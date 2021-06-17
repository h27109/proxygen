/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ProxyHandler.h"

#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/portability/GFlags.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>
#include <proxygen/lib/utils/URL.h>

#include "ProxyStats.h"

using namespace proxygen;
using std::string;
using std::unique_ptr;

DEFINE_int32(proxy_connect_timeout, 1000, "connect timeout in milliseconds");

namespace {
static const uint32_t kMinReadSize = 1460;
static const uint32_t kMaxReadSize = 4000;

static const uint8_t READS_SHUTDOWN = 1;
static const uint8_t WRITES_SHUTDOWN = 2;
static const uint8_t CLOSED = READS_SHUTDOWN | WRITES_SHUTDOWN;
}  // namespace

namespace ProxyService {

ProxyHandler::ProxyHandler(ProxyStats* stats, folly::HHWheelTimer* timer)
    : stats_(stats), connector_{this, timer}, serverHandler_(*this) {}

ProxyHandler::~ProxyHandler() { VLOG(4) << "deleting ProxyHandler"; }

// 收到http header后的回调函数
void ProxyHandler::onRequest(std::unique_ptr<HTTPMessage> headers) noexcept {
    // This HTTP proxy does not obey the rules in the spec, such as stripping
    // hop-by-hop headers.  Example only!
    stats_->recordRequest();
    request_ = std::move(headers);

    if (request_->getMethod() != HTTPMethod::GET && request_->getMethod() != HTTPMethod::POST) {
        ResponseBuilder(downstream_)
            .status(http_status_, "Bad Gateway")
            .body(rsp_body_)
            .sendWithEOM();
        return;
    }
}

// 收到http body后的回调函数
void ProxyHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
    // 缓存上游请求报文

}

// 服务端接收完毕
void ProxyHandler::onEOM() noexcept {
    ServiceRequest();

    if (!need_forward_) {
        ResponseBuilder(downstream_)
            .status(http_status_, "Bad Gateway")
            .body(rsp_body_)
            .sendWithEOM();
        return;
    }

    downstream_->pauseIngress();
    LOG(INFO) << "Trying to connect to " << addr;
    auto evb = folly::EventBaseManager::get()->getEventBase();
    const folly::SocketOptionMap opts{{{SOL_SOCKET, SO_REUSEADDR}, 1}};
    downstream_->pauseIngress();
    connector_.connect(folly::EventBaseManager::get()->getEventBase(), addr,
                       std::chrono::milliseconds(FLAGS_proxy_connect_timeout), opts);

    if (txn_) {
        LOG(INFO) << "Forwarding " << ((body) ? body->computeChainDataLength() : 0)
                  << " body bytes to server";
        txn_->sendBody(std::move(body));
    }

    if (txn_) {
        LOG(INFO) << "Forwarding client EOM to server";
        txn_->sendEOM();
    } /*else if (upstreamSock_) {
        LOG(INFO) << "Closing upgraded socket";
        sockStatus_ |= WRITES_SHUTDOWN;
        upstreamSock_->shutdownWrite();
    } */
    else {
        LOG(INFO) << "Dropping client EOM to server";
    }
}

void ProxyHandler::ServiceRequest() {
    proxygen::URL url(request_->getURL());
    std::string path = url.getPath();

    RequestHander request_handler;
    request_handler.SetReqHeaders().SetRequest();

    request_handler.DoRequest();
    need_forward_ = request_handler.GetForwardFlag();
    forward_url_ = request_handler.GetForwardUrl();
}

void ProxyHandler::ServiceResponse() {}

// upstream连接成功
void ProxyHandler::connectSuccess(HTTPUpstreamSession* session) {
    LOG(INFO) << "Established " << *session;
    session_ = std::make_unique<SessionWrapper>(session);
    txn_ = session->newTransaction(&serverHandler_);
    LOG(INFO) << "Forwarding client request: " << request_->getURL() << " to server";
    txn_->sendHeaders(*request_);
    downstream_->resumeIngress();
}

void ProxyHandler::connectError(const folly::AsyncSocketException& ex) {
    LOG(ERROR) << "Failed to connect: " << folly::exceptionStr(ex);
    if (!clientTerminated_) {
        ResponseBuilder(downstream_).status(503, "Bad Gateway").sendWithEOM();
    } else {
        abortDownstream();
        checkForShutdown();
    }
}

void ProxyHandler::onServerHeadersComplete(unique_ptr<HTTPMessage> msg) noexcept {
    CHECK(!clientTerminated_);
    LOG(INFO) << "Forwarding " << msg->getStatusCode() << " response to client";
    downstream_->sendHeaders(*msg);
}

void ProxyHandler::onServerBody(std::unique_ptr<folly::IOBuf> chain) noexcept {
    CHECK(!clientTerminated_);
    LOG(INFO) << "Forwarding " << ((chain) ? chain->computeChainDataLength() : 0)
              << " body bytes to client";
    // 缓存下游服务返回的报文
    // to do
    downstream_->sendBody(std::move(chain));
}

void ProxyHandler::onServerEOM() noexcept {
    // 处理下流服务返回的报文
    DoDownloadResponse();

    downstream_->sendBody(std::move(chain));

    if (!clientTerminated_) {
        LOG(INFO) << "Forwarding server EOM to client";
        downstream_->sendEOM();
    }
}

void ProxyHandler::detachServerTransaction() noexcept {
    txn_ = nullptr;
    checkForShutdown();
}

void ProxyHandler::onServerError(const HTTPException& error) noexcept {
    LOG(ERROR) << "Server error: " << error;
    abortDownstream();
}

void ProxyHandler::onServerEgressPaused() noexcept {
    if (!clientTerminated_) {
        downstream_->pauseIngress();
    }
}

void ProxyHandler::onServerEgressResumed() noexcept {
    if (!clientTerminated_) {
        downstream_->resumeIngress();
    }
}

void ProxyHandler::requestComplete() noexcept {
    clientTerminated_ = true;
    checkForShutdown();
}

void ProxyHandler::onError(ProxygenError err) noexcept {
    LOG(ERROR) << "Client error: " << proxygen::getErrorString(err);
    clientTerminated_ = true;
    if (txn_) {
        LOG(ERROR) << "Aborting server txn: " << *txn_;
        txn_->sendAbort();
    } else if (upstreamSock_) {
        upstreamSock_.reset();
    }
    checkForShutdown();
}

void ProxyHandler::onEgressPaused() noexcept {
    if (txn_) {
        txn_->pauseIngress();
    } else if (upstreamSock_) {
        upstreamSock_->setReadCB(nullptr);
    }
}

void ProxyHandler::onEgressResumed() noexcept {
    if (txn_) {
        txn_->resumeIngress();
    } else if (upstreamSock_) {
        upstreamSock_->setReadCB(this);
    }
}

void ProxyHandler::abortDownstream() {
    if (!clientTerminated_) {
        downstream_->sendAbort();
    }
}

bool ProxyHandler::checkForShutdown() {
    if (clientTerminated_ && !txn_ &&
        (!upstreamSock_ || (sockStatus_ == CLOSED && !upstreamEgressPaused_))) {
        delete this;
        return true;
    }
    return false;
}

void ProxyHandler::connectSuccess() noexcept {
    LOG(INFO) << "Connected to upstream " << upstreamSock_;
    ResponseBuilder(downstream_).status(200, "OK").send();
    upstreamSock_->setReadCB(this);
    downstream_->resumeIngress();
}

void ProxyHandler::connectErr(const folly::AsyncSocketException& ex) noexcept { connectError(ex); }

void ProxyHandler::getReadBuffer(void** bufReturn, size_t* lenReturn) {
    std::pair<void*, uint32_t> readSpace = body_.preallocate(kMinReadSize, kMaxReadSize);
    *bufReturn = readSpace.first;
    *lenReturn = readSpace.second;
}

void ProxyHandler::readDataAvailable(size_t len) noexcept {
    body_.postallocate(len);
    downstream_->sendBody(body_.move());
}

void ProxyHandler::readEOF() noexcept {
    sockStatus_ |= READS_SHUTDOWN;
    onServerEOM();
}

void ProxyHandler::readErr(const folly::AsyncSocketException& ex) noexcept {
    LOG(ERROR) << "Server read error: " << folly::exceptionStr(ex);
    abortDownstream();
    upstreamSock_.reset();
    checkForShutdown();
}

void ProxyHandler::writeSuccess() noexcept {
    upstreamEgressPaused_ = false;
    if (downstreamIngressPaused_) {
        downstreamIngressPaused_ = false;
        onServerEgressResumed();
    }
    checkForShutdown();
}

void ProxyHandler::writeErr(size_t /*bytesWritten*/,
                            const folly::AsyncSocketException& ex) noexcept {
    LOG(ERROR) << "Server write error: " << folly::exceptionStr(ex);
    ;
    upstreamEgressPaused_ = false;
    abortDownstream();
    upstreamSock_.reset();
    checkForShutdown();
}

}  // namespace ProxyService
