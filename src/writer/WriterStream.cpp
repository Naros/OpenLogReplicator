/* Thread writing to Network
   Copyright (C) 2018-2023 Adam Leszczynski (aleszczynski@bersler.com)

This file is part of OpenLogReplicator.

OpenLogReplicator is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 3, or (at your option)
any later version.

OpenLogReplicator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenLogReplicator; see the file LICENSE;  If not see
<http://www.gnu.org/licenses/>.  */

#include <unistd.h>

#include "../builder/Builder.h"
#include "../common/NetworkException.h"
#include "../common/OraProtoBuf.pb.h"
#include "../common/RuntimeException.h"
#include "../metadata/Metadata.h"
#include "../stream/Stream.h"
#include "WriterStream.h"

namespace OpenLogReplicator {
    WriterStream::WriterStream(Ctx* newCtx, const std::string& newAlias, const std::string& newDatabase, Builder* newBuilder, Metadata* newMetadata,
                               Stream* newStream) :
        Writer(newCtx, newAlias, newDatabase, newBuilder, newMetadata),
        stream(newStream) {
    }

    WriterStream::~WriterStream() {
        if (stream != nullptr) {
            delete stream;
            stream = nullptr;
        }
    }

    void WriterStream::initialize() {
        Writer::initialize();

        stream->initializeServer();
    }

    std::string WriterStream::getName() const {
        return stream->getName();
    }

    void WriterStream::readCheckpoint() {
        while (!streaming && !ctx->softShutdown) {
            try {
                usleep(ctx->pollIntervalUs);
                if (stream->isConnected()) {
                    pollQueue();
                }

            // client got disconnected
            } catch (NetworkException& ex) {
                ctx->warning(ex.code, ex.msg);
                streaming = false;
            }
        }

        if (metadata->firstDataScn != ZERO_SCN) {
            if (ctx->trace & TRACE_WRITER)
                ctx->logTrace(TRACE_WRITER, "client requested scn: " + std::to_string(metadata->firstDataScn));
        }
    }

    void WriterStream::processInfo() {
        ctx->info(0, "processInfo()");
        response.Clear();
        if (request.database_name() != database) {
            ctx->info(TRACE_WRITER, "client requested database: " + std::string(request.database_name()) + " instead of " + database);
            response.set_code(pb::ResponseCode::INVALID_DATABASE);
        } else if (metadata->firstDataScn != ZERO_SCN) {
            ctx->info(TRACE_WRITER, "client requested scn: " + std::to_string(metadata->firstDataScn) + " when already started");
            response.set_code(pb::ResponseCode::STARTED);
            response.set_scn(metadata->firstDataScn);
        } else {
            response.set_code(pb::ResponseCode::READY);
        }
    }

    void WriterStream::processStart() {
        ctx->info(0, "processStart()");
        response.Clear();
        if (request.database_name() != database) {
            ctx->info(TRACE_WRITER, "client requested database: " + std::string(request.database_name()) + " instead of " + database);
            response.set_code(pb::ResponseCode::INVALID_DATABASE);
            return;
        }

        if (metadata->firstDataScn != ZERO_SCN) {
            ctx->logTrace(TRACE_WRITER, "client requested scn: " + std::to_string(metadata->firstDataScn) + " when already started");
            response.set_code(pb::ResponseCode::ALREADY_STARTED);
            response.set_scn(metadata->firstDataScn);
            return;
        }

        metadata->startScn = 0;
        if (request.has_seq())
            metadata->startSequence = request.seq();
        else
            metadata->startSequence = ZERO_SEQ;
        metadata->startTime.clear();
        metadata->startTimeRel = 0;

        switch (request.tm_val_case()) {
            case pb::RedoRequest::TmValCase::kScn:
                metadata->startScn = request.scn();
                metadata->setStatusReplicate();
                break;

            case pb::RedoRequest::TmValCase::kTms:
                metadata->startTime = request.tms();
                metadata->setStatusReplicate();
                break;

            case pb::RedoRequest::TmValCase::kTmRel:
                metadata->startTimeRel = request.tm_rel();
                metadata->setStatusReplicate();
                break;

            default:
                ctx->logTrace(TRACE_WRITER, "client requested invalid tm: " + std::to_string(request.tm_val_case()));
                response.set_code(pb::ResponseCode::INVALID_COMMAND);
                break;
        }

        // TODO: wait for start

        // FIXME: the code is invalid here, the writer module should check if the Replicator
        // actually started and return proper information to the client
        // current workaround is to assume that it did start

        //if (metadata->firstDataScn != ZERO_SCN) {
            response.set_code(pb::ResponseCode::STARTED);
            response.set_scn(metadata->firstDataScn);
        //} else {
        //    ctx->logTrace(TRACE_WRITER, "client did not provide starting scn");
        //    response.set_code(pb::ResponseCode::FAILED_START);
        //}
    }

    void WriterStream::processRedo() {
        ctx->info(0, "processRedo()");
        response.Clear();
        if (request.database_name() == database) {
            response.set_code(pb::ResponseCode::STREAMING);
            ctx->info(0, "streaming to client");
            streaming = true;
        } else {
            ctx->info(TRACE_WRITER, "client requested database: " + std::string(request.database_name()) + " instead of " + database);
            response.set_code(pb::ResponseCode::INVALID_DATABASE);
        }
    }

    void WriterStream::processConfirm() {
        ctx->info(0, "processConfirm()");
        if (request.database_name() == database) {
            while (currentQueueSize > 0 && queue[0]->scn <= request.scn())
                confirmMessage(queue[0]);
        }
    }

    void WriterStream::pollQueue() {
        uint8_t msgR[READ_NETWORK_BUFFER];
        std::string msgS;

        uint64_t length = stream->receiveMessageNB(msgR, READ_NETWORK_BUFFER);

        if (length > 0) {
            request.Clear();
            if (request.ParseFromArray(msgR, static_cast<int>(length))) {
                if (streaming) {
                    switch (request.code()) {
                        case pb::RequestCode::INFO:
                            processInfo();
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            streaming = false;
                            break;

                        case pb::RequestCode::CONFIRM:
                            processConfirm();
                            break;

                        default:
                            ctx->warning(60032, "unknown request code: " + request.code());
                            response.Clear();
                            response.set_code(pb::ResponseCode::INVALID_COMMAND);
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            break;
                    }
                } else {
                    switch (request.code()) {

                        case pb::RequestCode::INFO:
                            processInfo();
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            break;

                        case pb::RequestCode::START:
                            processStart();
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            break;

                        case pb::RequestCode::REDO:
                            processRedo();
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            break;

                        default:
                            ctx->warning(60032, "unknown request code: " + request.code());
                            response.Clear();
                            response.set_code(pb::ResponseCode::INVALID_COMMAND);
                            response.SerializeToString(&msgS);
                            stream->sendMessage(msgS.c_str(), msgS.length());
                            break;
                    }
                }
            } else {
                std::ostringstream ss;
                ss << "request decoder[" << std::dec << length << "]: ";
                for (uint64_t i = 0; i < static_cast<uint64_t>(length); ++i)
                    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<uint64_t>(msgR[i]) << " ";
                ctx->warning(60033, ss.str());
            }

        } else if (length == 0) {
            // no request
        } else if (errno != EAGAIN)
            throw RuntimeException(10061, "network error, errno: " + std::to_string(errno) + ", message: " + strerror(errno));
    }

    void WriterStream::sendMessage(BuilderMsg* msg) {
        stream->sendMessage(msg->data, msg->length);
    }
}
