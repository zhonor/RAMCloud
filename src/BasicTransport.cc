/* Copyright (c) 2015-2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include <algorithm>

#include "BasicTransport.h"
#include "Service.h"
#include "ServiceLocator.h"
#include "TimeTrace.h"
#include "WireFormat.h"
#include "WorkerManager.h"

namespace RAMCloud {

// Change 0 -> 1 in the following line to compile detailed time tracing in
// this transport.
#define TIME_TRACE 0

// Provides a cleaner way of invoking TimeTrace::record, with the code
// conditionally compiled in or out by the TIME_TRACE #ifdef.
namespace {
    inline void
    timeTrace(const char* format,
            uint32_t arg0 = 0, uint32_t arg1 = 0, uint32_t arg2 = 0,
            uint32_t arg3 = 0)
    {
#if TIME_TRACE
        TimeTrace::record(format, arg0, arg1, arg2, arg3);
#endif
    }
}

/**
 * Construct a new BasicTransport.
 * 
 * \param context
 *      Shared state about various RAMCloud modules.
 * \param locator
 *      Service locator that contains parameters for this transport.
 * \param driver
 *      Used to send and receive packets. This transport becomes owner
 *      of the driver and will free it in when this object is deleted.
 * \param clientId
 *      Identifier that identifies us in outgoing RPCs: must be unique across
 *      all servers and clients.
 */
BasicTransport::BasicTransport(Context* context, const ServiceLocator* locator,
        Driver* driver, uint64_t clientId)
    : context(context)
    , driver(driver)
    , poller(context, this)
    , maxDataPerPacket(driver->getMaxPacketSize() - sizeof32(DataHeader))
    , clientId(clientId)
    , nextClientSequenceNumber(1)
    , nextServerSequenceNumber(1)
    , transmitSequenceNumber(1)
    , receivedPackets()
    , serverRpcPool()
    , clientRpcPool()
    , outgoingRpcs()
    , outgoingRequests()
    , incomingRpcs()
    , outgoingResponses()
    , serverTimerList()
    , roundTripBytes(getRoundTripBytes(locator))
    , grantIncrement(5*maxDataPerPacket)
    , timerInterval(0)
    , nextTimeoutCheck(0)
    , timeoutCheckDeadline(0)

    // As of 7/2016, the value for timeoutIntervals is set relatively high.
    // This is needed to handle issues on some machines (such as the NEC
    // Atom cluster) where threads can get descheduled by the kernel for
    // 10-30ms. This can result in delays in handling network packets, and
    // we don't want those delays to result in RPC timeouts.
    , timeoutIntervals(40)
    , pingIntervals(3)
{
    // Set up the timer to trigger at 2 ms intervals. We use this choice
    // (as of 11/2015) because the Linux kernel appears to buffer packets
    // for up to about 1 ms before delivering them to applications. Shorter
    // intervals result in unnecessary retransmissions.
    timerInterval = Cycles::fromMicroseconds(2000);
    nextTimeoutCheck = Cycles::rdtsc() + timerInterval;

    LOG(NOTICE, "BasicTransport parameters: maxDataPerPacket %u, "
            "roundTripBytes %u, grantIncrement %u, pingIntervals %d, "
            "timeoutIntervals %d, timerInterval %.2f ms",
            maxDataPerPacket, roundTripBytes,
            grantIncrement, pingIntervals, timeoutIntervals,
            Cycles::toSeconds(timerInterval)*1e3);
}

/**
 * Destructor for BasicTransports.
 */
BasicTransport::~BasicTransport()
{
    // This cleanup is mostly for the benefit of unit tests: in production,
    // this destructor is unlikely ever to get called.

    // Reclaim all of the RPC objects.
    for (ServerRpcMap::iterator it = incomingRpcs.begin();
            it != incomingRpcs.end(); ) {
        ServerRpc* serverRpc = it->second;

        // Advance iterator; otherwise it will get invalidated by
        // deleteServerRpc.
        it++;
        deleteServerRpc(serverRpc);
    }
    for (ClientRpcMap::iterator it = outgoingRpcs.begin();
            it != outgoingRpcs.end(); it++) {
        ClientRpc* clientRpc = it->second;
        deleteClientRpc(clientRpc);
    }
    delete driver;
}

// See Transport::getServiceLocator().
string
BasicTransport::getServiceLocator()
{
    return driver->getServiceLocator();
}

/*
 * When we are finished processing an outgoing RPC, this method is
 * invoked to delete the ClientRpc object and remove it from all
 * existing data structures.
 *
 * \param clientRpc
 *      An RPC that has either completed normally or is being
 *      aborted.
 */
void
BasicTransport::deleteClientRpc(ClientRpc* clientRpc)
{
    TEST_LOG("RpcId %lu", clientRpc->sequence);
    timeTrace("deleting client RPC, sequence %u",
            downCast<uint32_t>(clientRpc->sequence));
    outgoingRpcs.erase(clientRpc->sequence);
    if (clientRpc->transmitPending) {
        erase(outgoingRequests, *clientRpc);
    }
    clientRpcPool.destroy(clientRpc);
}

/*
 * When we are finished processing an incoming RPC, this method is
 * invoked to delete the RPC object and remove it from all existing
 * data structures.
 * 
 * \param serverRpc
 *      An RPC that has either completed normally or should be
 *      aborted.
 */
void
BasicTransport::deleteServerRpc(ServerRpc* serverRpc)
{
    TEST_LOG("RpcId (%lu, %lu)", serverRpc->rpcId.clientId,
            serverRpc->rpcId.sequence);
    timeTrace("deleting server RPC, sequence %u",
            downCast<uint32_t>(serverRpc->rpcId.sequence));
    incomingRpcs.erase(serverRpc->rpcId);
    if (serverRpc->sendingResponse) {
        erase(outgoingResponses, *serverRpc);
    }
    if (serverRpc->sendingResponse || !serverRpc->requestComplete) {
        erase(serverTimerList, *serverRpc);
    }
    serverRpcPool.destroy(serverRpc);
}

/**
 * Parse option values in a service locator to determine how many bytes
 * of data must be sent to cover the round-trip latency of a connection.
 *
 * \param locator
 *      Service locator that may contain "gbs" and "rttMicros" options.
 *      If NULL, or if any of the options  are missing, then defaults
 *      are supplied.  Note: as of 8/2016 these options don't work very well
 *      because they are only visible to servers, not clients.
 */
uint32_t
BasicTransport::getRoundTripBytes(const ServiceLocator* locator)
{
    uint32_t gBitsPerSec = 32;
    uint32_t roundTripMicros = 7;

    if (locator  != NULL) {
        if (locator->hasOption("gbs")) {
            char* end;
            uint32_t value = downCast<uint32_t>(strtoul(
                    locator->getOption("gbs").c_str(), &end, 10));
            if ((*end == 0) && (value != 0)) {
                gBitsPerSec = value;
            } else {
                LOG(ERROR, "Bad BasicTransport gbs option value '%s' "
                        "(expected positive integer); ignoring option",
                        locator->getOption("gbs").c_str());
            }
        }
        if (locator->hasOption("rttMicros")) {
            char* end;
            uint32_t value = downCast<uint32_t>(strtoul(
                    locator->getOption("rttMicros").c_str(), &end, 10));
            if ((*end == 0) && (value != 0)) {
                roundTripMicros = value;
            } else {
                LOG(ERROR, "Bad BasicTransport rttMicros option value '%s' "
                        "(expected positive integer); ignoring option",
                        locator->getOption("rttMicros").c_str());
            }
        }
    }

    // Compute round-trip time in terms of full packets (round up).
    uint32_t roundTripBytes = (roundTripMicros*gBitsPerSec*1000)/8;
    roundTripBytes = ((roundTripBytes+maxDataPerPacket-1)/maxDataPerPacket)
            * maxDataPerPacket;
    return roundTripBytes;
}

/**
 * Return a printable symbol for the opcode field from a packet.
 * \param opcode
 *     Opcode field from a packet.
 * \return
 *     The result is a static string, which may change on the next
 *
 */
string
BasicTransport::opcodeSymbol(uint8_t opcode) {
    switch (opcode) {
        case BasicTransport::PacketOpcode::ALL_DATA:
            return "ALL_DATA";
        case BasicTransport::PacketOpcode::DATA:
            return "DATA";
        case BasicTransport::PacketOpcode::GRANT:
            return "GRANT";
        case BasicTransport::PacketOpcode::LOG_TIME_TRACE:
            return "LOG_TIME_TRACE";
        case BasicTransport::PacketOpcode::RESEND:
            return "RESEND";
        case BasicTransport::PacketOpcode::ACK:
            return "ACK";
    }

    return format("%d", opcode);
}

/**
 * This method takes care of packet sizing and transmitting message data,
 * both for requests and for responses. When a method returns, the given
 * range of data will have been queued for the NIC but may not actually
 * have been transmitted yet.
 * 
 * \param address
 *      Identifies the destination for the message.
 * \param rpcId
 *      Unique identifier for the RPC.
 * \param message
 *      Contains the entire message.
 * \param offset
 *      Offset in bytes of the first byte to be transmitted.
 * \param maxBytes
 *      Maximum number of bytes to transmit. If offset + maxBytes exceeds
 *      the message length, then all of the remaining bytes in message,
 *      will be transmitted.
 * \param flags
 *      Extra flags to set in packet headers, such as FROM_CLIENT or
 *      RETRANSMISSION. Must at least specify either FROM_CLIENT or
 *      FROM_SERVER.
 * \param partialOK
 *      Normally, a partial packet will get sent only if it's the last
 *      packet in the message. However, if this parameter is true then
 *      partial packets will be sent anywhere in the message.
 * \return
 *      The number of bytes of data actually transmitted (may be 0 in
 *      some situations).
 */
uint32_t
BasicTransport::sendBytes(const Driver::Address* address, RpcId rpcId,
        Buffer* message, uint32_t offset, uint32_t maxBytes,
        uint8_t flags, bool partialOK)
{
    uint32_t messageSize = message->size();

    uint32_t curOffset = offset;
    uint32_t bytesSent = 0;
    while ((curOffset < messageSize) && (bytesSent < maxBytes)) {
        // Don't send less-than-full-size packets except for the last packet
        // of the message (unless the caller explicitly requested it).
        uint32_t bytesThisPacket =
                std::min(maxDataPerPacket, messageSize - curOffset);
        if ((bytesSent + bytesThisPacket) > maxBytes) {
            if (!partialOK) {
                break;
            }
            bytesThisPacket = maxBytes - bytesSent;
        }
        if (bytesThisPacket == messageSize) {
            // Entire message fits in a single packet.
            AllDataHeader header(rpcId, flags, downCast<uint16_t>(messageSize));
            Buffer::Iterator iter(message, 0, messageSize);
            driver->sendPacket(address, &header, &iter);
        } else {
            DataHeader header(rpcId, message->size(), curOffset, flags);
            Buffer::Iterator iter(message, curOffset, bytesThisPacket);
            driver->sendPacket(address, &header, &iter);
        }
        bytesSent += bytesThisPacket;
        curOffset += bytesThisPacket;
    }

    timeTrace("sent data, sequence %u, offset %u, length %u",
            downCast<uint32_t>(rpcId.sequence), offset, bytesSent);
    return bytesSent;
}

/**
 * Given a pointer to a BasicTransport packet, return a human-readable
 * string describing the information in its header.
 * 
 * \param packet
 *      Address of the first bite of the packet header, which must be
 *      contiguous in memory.
 * \param packetLength
 *      Size of the header, in bytes.
 */
string
BasicTransport::headerToString(const void* packet, uint32_t packetLength)
{
    string result;
    const BasicTransport::CommonHeader* common =
            static_cast<const BasicTransport::CommonHeader*>(packet);
    uint32_t headerLength = sizeof32(BasicTransport::CommonHeader);
    if (packetLength < headerLength) {
        goto packetTooShort;
    }
    result += BasicTransport::opcodeSymbol(common->opcode);
    if (common->flags & BasicTransport::FROM_CLIENT) {
        result += " FROM_CLIENT";
    } else {
        result += " FROM_SERVER";
    }
    result += format(", rpcId %lu.%lu",
            common->rpcId.clientId, common->rpcId.sequence);
    switch (common->opcode) {
        case BasicTransport::PacketOpcode::ALL_DATA:
            headerLength = sizeof32(BasicTransport::AllDataHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            break;
        case BasicTransport::PacketOpcode::DATA: {
            headerLength = sizeof32(BasicTransport::DataHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            const BasicTransport::DataHeader* data =
                    static_cast<const BasicTransport::DataHeader*>(packet);
            result += format(", totalLength %u, offset %u%s%s",
                    data->totalLength, data->offset,
                    common->flags & BasicTransport::NEED_GRANT
                            ? ", NEED_GRANT" : "",
                    common->flags & BasicTransport::RETRANSMISSION
                            ? ", RETRANSMISSION" : "");
            break;
        }
        case BasicTransport::PacketOpcode::GRANT: {
            headerLength = sizeof32(BasicTransport::GrantHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            const BasicTransport::GrantHeader* grant =
                    static_cast<const BasicTransport::GrantHeader*>(packet);
            result += format(", offset %u", grant->offset);
            break;
        }
        case BasicTransport::PacketOpcode::LOG_TIME_TRACE:
            headerLength = sizeof32(BasicTransport::LogTimeTraceHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            break;
        case BasicTransport::PacketOpcode::RESEND: {
            headerLength = sizeof32(BasicTransport::ResendHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            const BasicTransport::ResendHeader* resend =
                    static_cast<const BasicTransport::ResendHeader*>(
                    packet);
            result += format(", offset %u, length %u%s",
                    resend->offset, resend->length,
                    common->flags & BasicTransport::RESTART
                            ? ", RESTART" : "");
            break;
        }
        case BasicTransport::PacketOpcode::ACK:
            headerLength = sizeof32(BasicTransport::AckHeader);
            if (packetLength < headerLength) {
                goto packetTooShort;
            }
            break;
    }
    return result;

  packetTooShort:
    if (!result.empty()) {
        result += ", ";
    }
    result += format("packet too short (got %u bytes, need at least %u)",
            packetLength, headerLength);
    return result;
}

/**
 * This method queues one or more data packets for transmission, if (a) the
 * NIC queue isn't too long and (b) there is data that needs to be transmitted.
 * \return
 *      The return value is 1 if we actually transmitted one or more packets,
 *      zero otherwise.
 */
int
BasicTransport::tryToTransmitData()
{
    int result = 0;

    // Check to see if we can transmit any data packets. The overall goal
    // here is not to enqueue too many data packets at the NIC at once; this
    // allows us to preempt long messages with shorter ones, and data
    // packets with control packets. The code here only handles data packets;
    // control packets (and retransmitted data) are always passed to the
    // driver immediately.

    uint32_t transmitQueueSpace = static_cast<uint32_t>(std::max(0,
            driver->getTransmitQueueSpace(context->dispatch->currentTime)));
    uint32_t maxBytes;

    // Each iteration of the following loop transmits data packets for
    // a single request or response.
    while (transmitQueueSpace >= maxDataPerPacket) {
        // Find an outgoing request or response that is ready to transmit.
        // The policy here is as follows:
        // * For messages less than 10 KB, transmit the shortest message
        //   first.
        // * Longer messages are sent in FIFO order, and they get lower
        //   priority than short messages.
        // This code used to use "shortest first" across all messages,
        // but this resulted in spurious retransmissions when long messages
        // got preempted by other long messages: the receiver for the
        // preempted message thought packets must have been dropped.

        // Note: this code used to use std::maps instead of lists; the maps
        // were sorted by message length to avoid the cost of scanning
        // all RPCs. However, the maps had a very high insertion cost
        // (~50ns) even when empty, so it is faster overall to use lists.
        // If these lists were to become very long, then this decision made
        // need to be revisited.
        ClientRpc* clientRpc = NULL;
        ServerRpc* serverRpc = NULL;
        uint32_t minLength = ~0;
        uint64_t minSequence = ~0;
        for (OutgoingRequestList::iterator it = outgoingRequests.begin();
                    it != outgoingRequests.end(); it++) {
            ClientRpc* rpc = &(*it);
            if (rpc->transmitLimit <= rpc->transmitOffset) {
                // Can't transmit this message: waiting for grants.
                continue;
            }
            if ((rpc->request->size() < 10000)
                    ? (rpc->request->size() < minLength)
                    : ((minLength >= 10000)
                    && (rpc->transmitSequenceNumber < minSequence))) {
                minLength = rpc->request->size();
                minSequence = rpc->transmitSequenceNumber;
                clientRpc = rpc;
            }
        }
        for (OutgoingResponseList::iterator it = outgoingResponses.begin();
                    it != outgoingResponses.end(); it++) {
            ServerRpc* rpc = &(*it);
            if (rpc->transmitLimit <= rpc->transmitOffset) {
                // Can't transmit this message: waiting for grants.
                continue;
            }
            if ((rpc->replyPayload.size() < 10000)
                    ? (rpc->replyPayload.size() < minLength)
                    : ((minLength >= 10000)
                    && (rpc->transmitSequenceNumber < minSequence))) {
                minLength = rpc->replyPayload.size();
                minSequence = rpc->transmitSequenceNumber;
                serverRpc = rpc;
                clientRpc = NULL;
            }
        }

        if (clientRpc != NULL) {
            // Transmit one or more request DATA packets from clientRpc,
            // if appropriate.
            result = 1;
            maxBytes = std::min(transmitQueueSpace,
                    clientRpc->transmitLimit - clientRpc->transmitOffset);
            int bytesSent = sendBytes(
                    clientRpc->session->serverAddress,
                    RpcId(clientId, clientRpc->sequence),
                    clientRpc->request, clientRpc->transmitOffset,
                    maxBytes, FROM_CLIENT|clientRpc->needGrantFlag);
            assert(bytesSent > 0);     // Otherwise, infinite loop.
            clientRpc->transmitOffset += bytesSent;
            clientRpc->lastTransmitTime = Cycles::rdtsc();
            transmitQueueSpace -= bytesSent;
            if (clientRpc->transmitOffset >= clientRpc->request->size()) {
                erase(outgoingRequests, *clientRpc);
                clientRpc->transmitPending = false;
            }
        } else if (serverRpc != NULL) {
            // Transmit one or more response DATA packets from serverRpc,
            // if appropriate.
                result = 1;
            maxBytes = std::min(transmitQueueSpace,
                    serverRpc->transmitLimit - serverRpc->transmitOffset);
            int bytesSent = sendBytes(serverRpc->clientAddress,
                    serverRpc->rpcId, &serverRpc->replyPayload,
                    serverRpc->transmitOffset, maxBytes,
                    FROM_SERVER|serverRpc->needGrantFlag);
            assert(bytesSent > 0);     // Otherwise, infinite loop.
            serverRpc->transmitOffset += bytesSent;
            serverRpc->lastTransmitTime = Cycles::rdtsc();
            transmitQueueSpace -= bytesSent;
            if (serverRpc->transmitOffset >= serverRpc->replyPayload.size()) {
                // Delete the ServerRpc object as soon as we have transmitted
                // the last byte. This has the disadvantage that if some of
                // this data is lost we won't be able to retransmit it (the
                // whole RPC will be retried). However, this approach is
                // simpler and faster in the common case where data isn't lost.
                deleteServerRpc(serverRpc);
            }
        } else {
            // There are no messages with data that can be transmitted.
            break;
        }
    }

    return result;
}

/**
 * Construct a new client session.
 *
 * \throw TransportException
 *      The service locator couldn't be parsed (a log message will
 *      have been generated already).
 */
BasicTransport::Session::Session(BasicTransport* t,
        const ServiceLocator* locator, uint32_t timeoutMs)
    : Transport::Session(locator->getOriginalString())
    , t(t)
    , serverAddress(NULL)
    , aborted(false)
{
    try {
        serverAddress = t->driver->newAddress(locator);
    }
    catch (const Exception& e) {
        LOG(NOTICE, "%s", e.message.c_str());
        throw TransportException(HERE,
                "BasicTransport couldn't parse service locator");
    }
}

/*
 * Destructor for client sessions.
 */
BasicTransport::Session::~Session()
{
    abort();
    delete serverAddress;
}

// See Transport::Session::abort for docs.
void
BasicTransport::Session::abort()
{
    aborted = true;
    for (ClientRpcMap::iterator it = t->outgoingRpcs.begin();
            it != t->outgoingRpcs.end(); ) {
        ClientRpc* clientRpc = it->second;
        it++;
        if (clientRpc->session == this) {
            t->deleteClientRpc(clientRpc);
        }
    }
}

// See Transport::Session::cancelRequest for docs.
void
BasicTransport::Session::cancelRequest(RpcNotifier* notifier)
{
    for (ClientRpcMap::iterator it = t->outgoingRpcs.begin();
            it != t->outgoingRpcs.end(); it++) {
        ClientRpc* clientRpc = it->second;
        if (clientRpc->notifier == notifier) {
            t->deleteClientRpc(clientRpc);

            // It's no longer safe to use "it", but at this point we're
            // done (the RPC can't exist in the list twice).
            return;
        }
    }
}

// See Transport::Session::getRpcInfo for docs.
string
BasicTransport::Session::getRpcInfo()
{
    string result;
    for (ClientRpcMap::iterator it = t->outgoingRpcs.begin();
            it != t->outgoingRpcs.end(); it++) {
        ClientRpc* clientRpc = it->second;
        if (clientRpc->session != this) {
            continue;
        }
        if (result.size() != 0) {
            result += ", ";
        }
        result += WireFormat::opcodeSymbol(clientRpc->request);
    }
    if (result.empty())
        result = "no active RPCs";
    result += " to server at ";
    result += serviceLocator;
    return result;
}

// See Transport::Session::sendRequest for docs.
void
BasicTransport::Session::sendRequest(Buffer* request, Buffer* response,
                RpcNotifier* notifier)
{
    timeTrace("sendRequest invoked, sequence %u, length %u",
            downCast<uint32_t>(t->nextClientSequenceNumber),
            request->size());
    response->reset();
    if (aborted) {
        notifier->failed();
        return;
    }
    ClientRpc *clientRpc = t->clientRpcPool.construct(this,
            t->nextClientSequenceNumber, request, response, notifier);
    clientRpc->transmitLimit = t->roundTripBytes;
    if (clientRpc->transmitLimit < request->size()) {
        clientRpc->needGrantFlag = NEED_GRANT;
    }
    clientRpc->transmitSequenceNumber = t->transmitSequenceNumber;
    t->transmitSequenceNumber++;
    t->outgoingRpcs[t->nextClientSequenceNumber] = clientRpc;
    t->outgoingRequests.push_back(*clientRpc);
    clientRpc->transmitPending = true;
    t->nextClientSequenceNumber++;
    t->tryToTransmitData();
}

/**
 * This method is invoked whenever a packet arrives. It is the top-level
 * dispatching method for dealing with incoming packets, both for requests
 * and responses.
 * 
 * \param received
 *      Information about the new packet.
 */
void
BasicTransport::handlePacket(Driver::Received* received)
{
    // The following method retrieves a header from a packet
    CommonHeader* common = received->getOffset<CommonHeader>(0);
    if (common == NULL) {
        RAMCLOUD_CLOG(WARNING, "packet from %s too short (%u bytes)",
                received->sender->toString().c_str(), received->len);
        return;
    }

    if (!(common->flags & FROM_CLIENT)) {
        // This packet was sent by the server, and it pertains to an RPC
        // for which we are the client.
        ClientRpcMap::iterator it = outgoingRpcs.find(
                common->rpcId.sequence);
        if (it == outgoingRpcs.end()) {
            // We have no record of this RPC; most likely this packet
            // pertains to an earlier RPC that we've already finished
            // with (e.g., we might have sent a RESEND just before the
            // server since the response). Discard the packet.
            if (common->opcode == LOG_TIME_TRACE) {
                // For LOG_TIME_TRACE requests, dump the trace anyway.
                LOG(NOTICE, "Client received LOG_TIME_TRACE request from "
                        "server %s for (unknown) sequence %lu",
                        received->sender->toString().c_str(),
                        common->rpcId.sequence);
                TimeTrace::record(
                        "client received LOG_TIME_TRACE for sequence %u",
                        downCast<uint32_t>(common->rpcId.sequence));
                TimeTrace::printToLogBackground(context->dispatch);
            }
            TEST_LOG("Discarding unknown packet, sequence %lu",
                    common->rpcId.sequence);
            return;
        }
        ClientRpc* clientRpc = it->second;
        clientRpc->silentIntervals = 0;
        switch (common->opcode) {
            // ALL_DATA from server
            case PacketOpcode::ALL_DATA: {
                // This RPC is now finished.
                AllDataHeader* header = received->getOffset<AllDataHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                uint32_t length;
                char *payload = received->steal(&length);
                uint32_t requiredLength =
                        downCast<uint32_t>(header->messageLength) +
                        sizeof32(AllDataHeader);
                if (length < requiredLength) {
                    RAMCLOUD_CLOG(WARNING, "ALL_DATA response from %s too "
                            "short (got %u bytes, expected %u)",
                            received->sender->toString().c_str(),
                            length, requiredLength);
                    driver->release(payload);
                    return;
                }
                timeTrace("client received ALL_DATA, sequence %u, length %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        length);
                Driver::PayloadChunk::appendToBuffer(clientRpc->response,
                        payload + sizeof32(AllDataHeader),
                        header->messageLength, driver, payload);
                clientRpc->notifier->completed();
                deleteClientRpc(clientRpc);
                return;
            }

            // DATA from server
            case PacketOpcode::DATA: {
                DataHeader* header = received->getOffset<DataHeader>(0);
                bool retainPacket = false;
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("client received DATA, sequence %u, offset %u, "
                        "length %u, flags %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset, received->len, header->common.flags);
                if (!clientRpc->accumulator) {
                    clientRpc->accumulator.construct(this, clientRpc->response);
                }
                retainPacket = clientRpc->accumulator->addPacket(header,
                        received->len);
                if (clientRpc->response->size() >= header->totalLength) {
                    // Response complete.
                    if (clientRpc->response->size()
                            > header->totalLength) {
                        // We have more bytes than we want. This can happen
                        // if the last packet gets padded by the network
                        // layer to meet minimum size requirements. Just
                        // truncate the response.
                        clientRpc->response->truncate(header->totalLength);
                    }
                    clientRpc->notifier->completed();
                    deleteClientRpc(clientRpc);
                } else {
                    // See if we need to output a GRANT.
                    if ((header->common.flags & NEED_GRANT) &&
                            (clientRpc->grantOffset <
                            (clientRpc->response->size() + roundTripBytes)) &&
                            (clientRpc->grantOffset < header->totalLength)) {
                        clientRpc->grantOffset = clientRpc->response->size()
                                + roundTripBytes + grantIncrement;
                        timeTrace(
                                "client sending GRANT, sequence %u, offset %u",
                                downCast<uint32_t>(
                                header->common.rpcId.sequence),
                                clientRpc->grantOffset);
                        GrantHeader grant(header->common.rpcId,
                                clientRpc->grantOffset, FROM_CLIENT);
                        driver->sendPacket(clientRpc->session->serverAddress,
                                &grant, NULL);
                    }
                }
                if (retainPacket) {
                    uint32_t dummy;
                    received->steal(&dummy);
                }
                return;
            }

            // GRANT from server
            case PacketOpcode::GRANT: {
                GrantHeader* header = received->getOffset<GrantHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("client received GRANT, sequence %u, offset %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset);
                if (header->offset > clientRpc->transmitLimit) {
                    clientRpc->transmitLimit = header->offset;
                }
                return;
            }

            // LOG_TIME_TRACE from server
            case PacketOpcode::LOG_TIME_TRACE: {
                LOG(NOTICE, "Client received LOG_TIME_TRACE request from "
                        "server %s for sequence %lu",
                        received->sender->toString().c_str(),
                        common->rpcId.sequence);
                TimeTrace::record(
                        "client received LOG_TIME_TRACE for sequence %u",
                        downCast<uint32_t>(common->rpcId.sequence));
                TimeTrace::printToLogBackground(context->dispatch);
                return;
            }

            // RESEND from server
            case PacketOpcode::RESEND: {
                ResendHeader* header = received->getOffset<ResendHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("client received RESEND, sequence %u, offset %u, "
                        "length %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset, header->length);
                if (header->common.flags & RESTART) {
                    clientRpc->response->reset();
                    clientRpc->transmitOffset = 0;
                    clientRpc->transmitLimit = header->length;
                    clientRpc->grantOffset = 0;
                    clientRpc->resendLimit = 0;
                    clientRpc->accumulator.destroy();
                    if (!clientRpc->transmitPending) {
                        clientRpc->transmitPending = true;
                        outgoingRequests.push_back(*clientRpc);
                    }
                    return;
                }
                uint32_t resendEnd = header->offset + header->length;
                if (resendEnd > clientRpc->transmitLimit) {
                    // Needed in case a GRANT packet was lost.
                    clientRpc->transmitLimit = resendEnd;
                }
                if ((header->offset >= clientRpc->transmitOffset)
                        || ((Cycles::rdtsc() - clientRpc->lastTransmitTime)
                        < timerInterval)) {
                    // One of two things has happened: either (a) we haven't
                    // yet sent the requested bytes for the first time (there
                    // must be other outgoing traffic with higher priority)
                    // or (b) we transmitted data recently. In either case,
                    // it's unlikely that bytes have been lost, so don't
                    // retransmit; just return an ACK so the server knows
                    // we're still alive.
                    AckHeader ack(header->common.rpcId, FROM_CLIENT);
                    driver->sendPacket(clientRpc->session->serverAddress,
                            &ack, NULL);
                    return;

                }
                double elapsedMicros = Cycles::toSeconds(Cycles::rdtsc()
                        - clientRpc->lastTransmitTime)*1e06;
                RAMCLOUD_CLOG(NOTICE, "Retransmitting to server %s: "
                        "sequence %lu, offset %u, length %u, elapsed "
                        "time %.1f us",
                        received->sender->toString().c_str(),
                        header->common.rpcId.sequence, header->offset,
                        header->length, elapsedMicros);
                sendBytes(clientRpc->session->serverAddress,
                        header->common.rpcId, clientRpc->request,
                        header->offset, header->length,
                        FROM_CLIENT|RETRANSMISSION|clientRpc->needGrantFlag,
                        true);
                clientRpc->lastTransmitTime = Cycles::rdtsc();
                return;
            }

            // ACK from server
            case PacketOpcode::ACK: {
                // Nothing to do.
                timeTrace("client received ACK, sequence %u",
                        downCast<uint32_t>(common->rpcId.sequence));
                return;
            }

            default:
            RAMCLOUD_CLOG(WARNING,
                    "unexpected opcode %s received from server %s",
                    opcodeSymbol(common->opcode).c_str(),
                    received->sender->toString().c_str());
            return;
        }
    } else {
        // This packet was sent by the client; it relates to an RPC
        // for which we are the server.

        // Find the record for this RPC, if one exists.
        ServerRpc* serverRpc = NULL;
        ServerRpcMap::iterator it = incomingRpcs.find(common->rpcId);
        if (it != incomingRpcs.end()) {
            serverRpc = it->second;
            serverRpc->silentIntervals = 0;
        }

        switch (common->opcode) {
            // ALL_DATA from client
            case PacketOpcode::ALL_DATA: {
                // Common case: the entire request fit in a single packet.

                AllDataHeader* header = received->getOffset<AllDataHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("server received ALL_DATA, sequence %u, length %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->messageLength);
                if (serverRpc != NULL) {
                    // This shouldn't normally happen: it means this packet is
                    // a duplicate, so we can just discard it.
                    return;
                }
                uint32_t length;
                char *payload = received->steal(&length);
                uint32_t requiredLength =
                        downCast<uint32_t>(header->messageLength) +
                        sizeof32(AllDataHeader);
                if (length < requiredLength) {
                    RAMCLOUD_CLOG(WARNING, "ALL_DATA request from %s too "
                            "short (got %u bytes, expected %u)",
                            received->sender->toString().c_str(),
                            length, requiredLength);
                    driver->release(payload);
                    return;
                }
                serverRpc = serverRpcPool.construct(this,
                        nextServerSequenceNumber, received->sender,
                        header->common.rpcId);
                nextServerSequenceNumber++;
                incomingRpcs[header->common.rpcId] = serverRpc;
                Driver::PayloadChunk::appendToBuffer(&serverRpc->requestPayload,
                        payload + sizeof32(AllDataHeader),
                        header->messageLength, driver, payload);
                serverRpc->requestComplete = true;
                context->workerManager->handleRpc(serverRpc);
                return;
            }

            // DATA from client
            case PacketOpcode::DATA: {
                DataHeader* header = received->getOffset<DataHeader>(0);
                bool retainPacket = false;
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("server received DATA, sequence %u, offset %u, "
                        "length %u, flags %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset, received->len, header->common.flags);
                if (serverRpc == NULL) {
                    serverRpc = serverRpcPool.construct(this,
                            nextServerSequenceNumber, received->sender,
                            header->common.rpcId);
                    nextServerSequenceNumber++;
                    incomingRpcs[header->common.rpcId] = serverRpc;
                    serverRpc->accumulator.construct(this,
                            &serverRpc->requestPayload);
                    serverTimerList.push_back(*serverRpc);
                } else if (serverRpc->requestComplete) {
                    // We've already received the full message, so
                    // ignore this packet.
                    TEST_LOG("ignoring extraneous packet");
                    goto serverDataDone;
                }
                retainPacket = serverRpc->accumulator->addPacket(header,
                        received->len);
                if (header->offset == 0) {
                    timeTrace("server received opcode %u, totalLength %u",
                            serverRpc->requestPayload.getStart<
                            WireFormat::RequestCommon>()->opcode,
                            header->totalLength);
                }
                if (serverRpc->requestPayload.size() >= header->totalLength) {
                    // Message complete; start servicing the RPC.
                    if (serverRpc->requestPayload.size()
                            > header->totalLength) {
                        // We have more bytes than we want. This can happen
                        // if the last packet gets padded by the network
                        // layer to meet minimum size requirements. Just
                        // truncate the request.
                        serverRpc->requestPayload.truncate(header->totalLength);
                    }
                    erase(serverTimerList, *serverRpc);
                    serverRpc->requestComplete = true;
                    context->workerManager->handleRpc(serverRpc);
                } else {
                    // See if we need to output a GRANT.
                    if ((header->common.flags & NEED_GRANT) &&
                            (serverRpc->grantOffset <
                            (serverRpc->requestPayload.size()
                            + roundTripBytes)) &&
                            (serverRpc->grantOffset < header->totalLength)) {
                        serverRpc->grantOffset =
                                serverRpc->requestPayload.size()
                                + roundTripBytes + grantIncrement;
                        timeTrace(
                                "server sending GRANT, sequence %u, offset %u",
                                downCast<uint32_t>(
                                header->common.rpcId.sequence),
                                serverRpc->grantOffset);
                        GrantHeader grant(header->common.rpcId,
                                serverRpc->grantOffset, FROM_SERVER);
                        driver->sendPacket(serverRpc->clientAddress,
                                &grant, NULL);
                    }
                }
                serverDataDone:
                if (retainPacket) {
                    uint32_t dummy;
                    received->steal(&dummy);
                }
                return;
            }

            // GRANT from client
            case PacketOpcode::GRANT: {
                GrantHeader* header = received->getOffset<GrantHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("server received GRANT, sequence %u, offset %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset);
                if ((serverRpc == NULL) || !serverRpc->sendingResponse) {
                    RAMCLOUD_LOG(WARNING, "unexpected GRANT from client %s, "
                            "id (%lu,%lu), grantOffset %u",
                            received->sender->toString().c_str(),
                            header->common.rpcId.clientId,
                            header->common.rpcId.sequence, header->offset);
                    return;
                }
                if (header->offset > serverRpc->transmitLimit) {
                    serverRpc->transmitLimit = header->offset;
                }
                return;
            }

            // LOG_TIME_TRACE from client
            case PacketOpcode::LOG_TIME_TRACE: {
                LOG(NOTICE, "Server received LOG_TIME_TRACE request from "
                        "client %s for sequence %lu",
                        received->sender->toString().c_str(),
                        common->rpcId.sequence);
                TimeTrace::record(
                        "server received LOG_TIME_TRACE for sequence %u",
                        downCast<uint32_t>(common->rpcId.sequence));
                TimeTrace::printToLogBackground(context->dispatch);
                return;
            }

            // RESEND from client
            case PacketOpcode::RESEND: {
                ResendHeader* header = received->getOffset<ResendHeader>(0);
                if (header == NULL)
                    goto packetLengthError;
                timeTrace("server received RESEND, sequence %u, offset %u, "
                        "length %u",
                        downCast<uint32_t>(header->common.rpcId.sequence),
                        header->offset, header->length);

                if (serverRpc == NULL) {
                    // This situation can happen if we never received the
                    // request, or if a packet of the response got lost but
                    // we have already freed the ServerRpc. In either case,
                    // ask the client to restart the RPC from scratch.
                    timeTrace("server requesting restart, sequence %u",
                            downCast<uint32_t>(common->rpcId.sequence));
                    ResendHeader resend(header->common.rpcId, 0,
                            roundTripBytes, FROM_SERVER|RESTART);
                    driver->sendPacket(received->sender, &resend, NULL);
                    return;
                }
                uint32_t resendEnd = header->offset + header->length;
                if (resendEnd > serverRpc->transmitLimit) {
                    // Needed in case GRANT packet was lost.
                    serverRpc->transmitLimit = resendEnd;
                }
                if (!serverRpc->sendingResponse
                        || (header->offset >= serverRpc->transmitOffset)
                        || ((Cycles::rdtsc() - serverRpc->lastTransmitTime)
                        < timerInterval)) {
                    // One of two things has happened: either (a) we haven't
                    // yet sent the requested bytes for the first time (there
                    // must be other outgoing traffic with higher priority)
                    // or (b) we transmitted data recently, so it might have
                    // crossed paths with the RESEND request. In either case,
                    // it's unlikely that bytes have been lost, so don't
                    // retransmit; just return an ACK so the client knows
                    // we're still alive.
                    AckHeader ack(serverRpc->rpcId, FROM_SERVER);
                    driver->sendPacket(serverRpc->clientAddress,
                            &ack, NULL);
                    return;
                }
                double elapsedMicros = Cycles::toSeconds(Cycles::rdtsc()
                        - serverRpc->lastTransmitTime)*1e06;
                RAMCLOUD_CLOG(NOTICE, "Retransmitting to client %s: "
                        "sequence %lu, offset %u, length %u, elapsed "
                        "time %.1f us",
                        received->sender->toString().c_str(),
                        header->common.rpcId.sequence, header->offset,
                        header->length, elapsedMicros);
                sendBytes(serverRpc->clientAddress,
                        serverRpc->rpcId, &serverRpc->replyPayload,
                        header->offset, header->length,
                        RETRANSMISSION|FROM_SERVER|serverRpc->needGrantFlag,
                        true);
                serverRpc->lastTransmitTime = Cycles::rdtsc();
                return;
            }

            // ACK from client
            case PacketOpcode::ACK: {
                // Nothing to do.
                timeTrace("server received ACK, sequence %u",
                        downCast<uint32_t>(common->rpcId.sequence));
                return;
            }

            default:
                RAMCLOUD_CLOG(WARNING,
                        "unexpected opcode %s received from client %s",
                        opcodeSymbol(common->opcode).c_str(),
                        received->sender->toString().c_str());
                return;
        }
    }

    packetLengthError:
    RAMCLOUD_CLOG(WARNING, "packet of type %s from %s too short (%u bytes)",
            opcodeSymbol(common->opcode).c_str(),
            received->sender->toString().c_str(),
            received->len);

}

/**
 * Returns a string containing human-readable information about the client
 * that initiated this RPC. Right now this isn't formatted as a service
 * locator; it just describes a Driver::Address.
 */
string
BasicTransport::ServerRpc::getClientServiceLocator()
{
    return clientAddress->toString();
}

/**
 * This method is invoked when a server has finished processing an RPC.
 * It begins transmitting the response back to the client, but returns
 * before that process is complete.
 */
void
BasicTransport::ServerRpc::sendReply()
{
    timeTrace("sendReply invoked, sequence %u, length %u",
            downCast<uint32_t>(rpcId.sequence), replyPayload.size());
    sendingResponse = true;
    transmitLimit = t->roundTripBytes;
    if (transmitLimit < replyPayload.size()) {
        needGrantFlag = NEED_GRANT;
    }
    transmitSequenceNumber = t->transmitSequenceNumber;
    t->transmitSequenceNumber++;
    t->outgoingResponses.push_back(*this);
    t->serverTimerList.push_back(*this);
    t->tryToTransmitData();
}

/**
 * Construct a MessageAccumulator.
 *
 * \param t
 *      Overall information about the transport.
 * \param buffer
 *      The complete message will be assembled here; caller should ensure
 *      that this is initially empty. The caller owns the storage for this
 *      and must ensure that it persists as long as this object persists.
 */
BasicTransport::MessageAccumulator::MessageAccumulator(BasicTransport* t,
        Buffer* buffer)
    : t(t)
    , buffer(buffer)
    , fragments()
    , grantOffset(0)
{ }

/**
 * Destructor for MessageAccumulators.
 */
BasicTransport::MessageAccumulator::~MessageAccumulator()
{
    // If there are any unassembled fragments, then we must release
    // them back to the driver.
    for (FragmentMap::iterator it = fragments.begin();
            it != fragments.end(); it++) {
        MessageFragment fragment = it->second;
        t->driver->release(fragment.header);
    }
    fragments.clear();
}

/**
 * This method is invoked whenever a new DATA packet arrives for a partially
 * complete message. It saves information about the new fragment and
 * (eventually) combines all of the fragments into a complete message.
 * 
 * \param header
 *      Pointer to the first byte of the packet, which must be a valid
 *      DATA packet.
 * \param length
 *      Total number of bytes in the packet.
 * \return
 *      The return value is true if we have retained a pointer to the
 *      packet (meaning that the caller should "steal" the Received, if
 *      it hasn't already). False means that the data in this packet
 *      was all redundant; we didn't save anything, so the caller need
 *      not steal the Received.
 */
bool
BasicTransport::MessageAccumulator::addPacket(DataHeader *header,
        uint32_t length)
{
    if (header->offset > buffer->size()) {
        // Can't append this packet into the buffer because some prior
        // data is missing. Save the packet for later.
        fragments[header->offset] = MessageFragment(header, length);
        return true;
    }

    // Append this fragment to the assembled message buffer, then see
    // if some of the unappended fragments can now be appended as well.
    bool result = appendFragment(header, length);
    while (true) {
        FragmentMap::iterator it = fragments.begin();
        if (it == fragments.end()) {
            break;
        }
        uint32_t offset = it->first;
        if (offset > buffer->size()) {
            break;
        }
        MessageFragment fragment = it->second;
        if (!appendFragment(fragment.header, fragment.length)) {
            t->driver->release(fragment.header);
        };
        fragments.erase(it);
    }
    return result;
}

/**
 * This method is invoked to append a fragment to a partially-assembled
 * message. It handles the special cases where part or all of the
 * fragment is already in the assembled message.
 * 
 * \param header
 *      Address of the first byte of a DATA packet.
 * \param length
 *      Total size of the packet at *header (including header).
 * \return
 *      True means that (some of) the data in this fragment was
 *      incorporated into the message buffer. False means that
 *      the data in this fragment is entirely redundant, so we
 *      didn't save any pointers to it (the caller may want to
 *      free this packet).
 */
bool
BasicTransport::MessageAccumulator::appendFragment(DataHeader *header,
        uint32_t length)
{
    uint32_t bytesToSkip = buffer->size() - header->offset;
    length -= sizeof32(DataHeader);
    if (bytesToSkip >= length) {
        // This entire fragment is redundant.
        return false;
    }
    Driver::PayloadChunk::appendToBuffer(buffer,
            reinterpret_cast<char*>(header) + sizeof32(DataHeader)
            + bytesToSkip, length - bytesToSkip, t->driver,
            reinterpret_cast<char*>(header));
    return true;
}

/**
 * This method is invoked to issue a RESEND packet when it appears that
 * packets have been lost. It is used by both servers and clients.
 * 
 * \param t
 *      Overall information about the transport.
 * \param address
 *      Network address to which the RESEND should be sent.
 * \param rpcId
 *      Unique identifier for the RPC in question.
 * \param grantOffset
 *      Largest grantOffset that we have sent for this message (i.e.
 *      this is how many total bytes we should have received already).
 *      May be 0 if the client never requested a grant (meaning that it
 *      planned to transmit the entire message unilaterally).
 * \param roundTripBytes
 *      Number of bytes that can be transmitted during the time it takes
 *      for a round-trip latency.
 * \param whoFrom
 *      Must be either FROM_CLIENT, indicating that we are the client, or
 *      FROM_SERVER, indicating that we are the server.
 *
 * \return
 *      The offset of the byte just after the last one whose retransmission
 *      was requested.
 */
uint32_t
BasicTransport::MessageAccumulator::requestRetransmission(BasicTransport *t,
        const Driver::Address* address, RpcId rpcId, uint32_t grantOffset,
        uint32_t roundTripBytes, uint8_t whoFrom)
{
    if ((reinterpret_cast<uint64_t>(&fragments) < 0x1000lu)) {
        DIE("Bad fragment pointer: %p", &fragments);
    }
    uint32_t endOffset;
    FragmentMap::iterator it = fragments.begin();

    // Compute the end of the retransmission range.
    if (it != fragments.end()) {
        // Retransmit the entire gap up to the first fragment.
        endOffset = it->first;
    } else if (grantOffset > 0) {
        // Retransmit everything that we've asked the sender to send:
        // we don't seem to have received any of it.
        endOffset = grantOffset;
    } else {
        // We haven't issued a GRANT for this message; just request
        // the first round-trip's worth of data. Once this data arrives,
        // the normal grant mechanism should kick in if it's still needed.
        endOffset = roundTripBytes;
    }
    assert(endOffset > buffer->size());
    if (whoFrom == FROM_SERVER) {
        timeTrace("server requesting retransmission of bytes %u-%u, "
                "sequence %u", buffer->size(), endOffset,
                downCast<uint32_t>(rpcId.sequence));
    } else {
        timeTrace("client requesting retransmission of bytes %u-%u, "
                "sequence %u", buffer->size(), endOffset,
                downCast<uint32_t>(rpcId.sequence));
    }
    ResendHeader resend(rpcId, buffer->size(), endOffset - buffer->size(),
            whoFrom);
    t->driver->sendPacket(address, &resend, NULL);
    return endOffset;
}

/**
 * This method is invoked in the inner polling loop of the dispatcher;
 * it drives the operation of the transport.
 * \return
 *      The return value is 1 if this method found something useful to do,
 *      0 otherwise.
 */
int
BasicTransport::Poller::poll()
{
    int result = 0;

    // Process any available incoming packets.
#define MAX_PACKETS 8
    t->driver->receivePackets(MAX_PACKETS, &t->receivedPackets);
    int numPackets = downCast<int>(t->receivedPackets.size());
    for (int i = 0; i < numPackets; i++) {
        result = 1;
        t->handlePacket(&t->receivedPackets[i]);
    }

    // See if we should check for timeouts. Ideally, we'd like to do this
    // every timerInterval. However, it's better not to call checkTimeouts
    // when there are input packets pending, since checkTimeouts might then
    // request retransmission of a packet that's waiting in the NIC. Thus,
    // if necessary, we delay the call to checkTimeouts to find a time when
    // we are caught up on input packets. If a long time goes by without ever
    // catching up, then we invoke checkTimeouts anyway.
    //
    // Note: it isn't a disaster if we occasionally request an unnecessary
    // retransmission, since the protocol will handle this fine. However, if
    // too many of these happen, it will create noise in the logs, which will
    // make it harder to notice when a *real* problem happens. Thus, it's
    // best to eliminate spurious retransmissions as much as possible.
    uint64_t now = Cycles::rdtsc();
    if (now >= t->nextTimeoutCheck) {
        if (t->timeoutCheckDeadline == 0) {
            t->timeoutCheckDeadline = now + t->timerInterval;
        }
        if ((numPackets < MAX_PACKETS)
                || (now >= t->timeoutCheckDeadline)) {
            if (numPackets == MAX_PACKETS) {
                RAMCLOUD_CLOG(NOTICE, "Deadline invocation of checkTimeouts");
                timeTrace("Deadline invocation of checkTimeouts");
            }
            t->checkTimeouts();
            result = 1;
            t->nextTimeoutCheck = now + t->timerInterval;
            t->timeoutCheckDeadline = 0;
        }
    }

    // Transmit data packets if possible.
    result |= t->tryToTransmitData();

    t->receivedPackets.clear();
    return result;
}

/**
 * This method is invoked by poll at regular intervals to check for
 * unexpected lapses in communication. It implements all of the timer-related
 * functionality for both clients and servers, such as requesting packet
 * retransmission and aborting RPCs.
 */
void
BasicTransport::checkTimeouts()
{
    // Scan all of the ClientRpc objects.
    for (ClientRpcMap::iterator it = outgoingRpcs.begin();
            it != outgoingRpcs.end(); ) {
        uint64_t sequence = it->first;
        ClientRpc* clientRpc = it->second;
        if (clientRpc->transmitOffset == 0) {
            // We haven't started transmitting this RPC yet (our transmit
            // queue is probably backed up), so no need to worry about whether
            // we have heard from the server.
            it++;
            continue;
        }
        clientRpc->silentIntervals++;

        // Advance the iterator here, so that it won't get invalidated if
        // we delete the ClientRpc below.
        it++;

        assert(timeoutIntervals > 2*pingIntervals);
        if (clientRpc->silentIntervals >= timeoutIntervals) {
            // A long time has elapsed with no communication whatsoever
            // from the server, so abort the RPC.
            RAMCLOUD_LOG(WARNING, "aborting %s RPC to server %s, "
                    "sequence %lu: timeout",
                    WireFormat::opcodeSymbol(clientRpc->request),
                    clientRpc->session->serverAddress->toString().c_str(),
                    sequence);
            clientRpc->notifier->failed();
            deleteClientRpc(clientRpc);
            continue;
        }

        if (clientRpc->response->size() == 0) {
            // We haven't received any part of the response message.
            // Send occasional RESEND packets, which should produce some
            // response from the server, so that we know it's still alive
            // and working. Note: the wait time for this ping is longer
            // than the server's wait time to request retransmission (first
            // give the server a chance to handle the problem).
            if ((clientRpc->silentIntervals % pingIntervals) == 0) {
                timeTrace("client sending RESEND for sequence %u",
                        downCast<uint32_t>(sequence));
                ResendHeader resend(RpcId(clientId, sequence), 0,
                        roundTripBytes, FROM_CLIENT);
                // The RESEND packet is effectively a grant...
                clientRpc->grantOffset = roundTripBytes;
                driver->sendPacket(clientRpc->session->serverAddress,
                        &resend, NULL);
            }
        } else {
            // We have received part of the response. If the server has gone
            // silent, this must mean packets were lost, so request
            // retransmission.
            assert(clientRpc->accumulator);
            if (clientRpc->silentIntervals >= 2) {
                clientRpc->resendLimit =
                        clientRpc->accumulator->requestRetransmission(this,
                        clientRpc->session->serverAddress,
                        RpcId(clientId, sequence),
                        clientRpc->grantOffset, roundTripBytes, FROM_CLIENT);
            }
        }
    }

    // Scan all of the ServerRpc objects for which network I/O is in
    // progress (either for the request or the response).
    for (ServerTimerList::iterator it = serverTimerList.begin();
            it != serverTimerList.end(); ) {
        ServerRpc* serverRpc = &(*it);
        if (serverRpc->sendingResponse && (serverRpc->transmitOffset == 0)) {
            // Looks like the transmit queue has been too backed up to start
            // sending the response, so no need to check for a timeout.
            it++;
            continue;
        }
        serverRpc->silentIntervals++;

        // Advance the iterator now, so it won't get invalidated if we
        // delete the ServerRpc below.
        it++;

        // If a long time has elapsed with no communication whatsoever
        // from the client, then abort the RPC. Note: this code should
        // only be executed when we're waiting to transmit or receive
        // (never if we're waiting for the RPC to execute locally).
        // The most common reasons for getting here are:
        // (a) The client has crashed
        // (b) The client sent us data for an RPC after we processed it,
        //     returned the result, and deleted the RPC; as a result, we
        //     created a new RPC that the client no longer cares about.
        //     Such extraneous data can occur if we requested a
        //     retransmission but then the original data arrived, so we
        //     could process the RPC before the retransmitted data arrived.
        assert(serverRpc->sendingResponse || !serverRpc->requestComplete);
        if (serverRpc->silentIntervals >= timeoutIntervals) {
            deleteServerRpc(serverRpc);
            continue;
        }

        // See if we need to request retransmission for part of the request
        // message.
        if ((serverRpc->silentIntervals >= 2) && !serverRpc->requestComplete) {
            serverRpc->resendLimit =
                    serverRpc->accumulator->requestRetransmission(this,
                    serverRpc->clientAddress, serverRpc->rpcId,
                    serverRpc->grantOffset, roundTripBytes, FROM_SERVER);
        }
    }
}

}  // namespace RAMCloud
