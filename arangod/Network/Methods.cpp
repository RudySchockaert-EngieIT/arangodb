////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "Methods.h"

#include "Agency/AgencyFeature.h"
#include "Agency/Agent.h"
#include "Basics/Common.h"
#include "Basics/HybridLogicalClock.h"
#include "Cluster/ServerState.h"
#include "Futures/Utilities.h"
#include "Network/ConnectionPool.h"
#include "Network/NetworkFeature.h"
#include "Network/Utils.h"

#include <fuerte/connection.h>
#include <fuerte/requests.h>
#include <fuerte/types.h>

#include "Scheduler/Scheduler.h"
#include "Scheduler/SchedulerFeature.h"

namespace arangodb {
namespace network {
using namespace arangodb::fuerte;
  
using PromiseRes = arangodb::futures::Promise<network::Response>;
  
template<typename T>
auto prepareRequest(RestVerb type, std::string const& path, T&& payload,
                    Timeout timeout, Headers const& headers) {
  
  fuerte::StringMap params; // intentionally empty
  auto req = fuerte::createRequest(type, path, params, std::forward<T>(payload));
  req->header.parseArangoPath(path); // strips /_db/<name>/
  if (req->header.database.empty()) {
    req->header.database = StaticStrings::SystemDatabase;
  }
  req->header.addMeta(headers);
  
  TRI_voc_tick_t timeStamp = TRI_HybridLogicalClock();
  req->header.addMeta(StaticStrings::HLCHeader,
                      arangodb::basics::HybridLogicalClock::encodeTimeStamp(timeStamp));
  
  req->timeout(std::chrono::duration_cast<std::chrono::milliseconds>(timeout));

  auto state = ServerState::instance();
  if (state->isCoordinator() || state->isDBServer()) {
    req->header.addMeta(StaticStrings::ClusterCommSource, state->getId());
  } else if (state->isAgent()) {
    auto agent = AgencyFeature::AGENT;
    if (agent != nullptr) {
      req->header.addMeta(StaticStrings::ClusterCommSource, "AGENT-" + agent->id());
    }
  }
  
  return req;
}

/// @brief send a request to a given destination
FutureRes sendRequest(DestinationId const& destination, RestVerb type,
                      std::string const& path, velocypack::Buffer<uint8_t> payload,
                      Timeout timeout, Headers const& headers) {
  // FIXME build future.reset(..)
  
  ConnectionPool* pool = NetworkFeature::pool();
  if (!pool) {
    LOG_TOPIC(ERR, Logger::FIXME) << "connection pool unavailble";
    return futures::makeFuture(Response{destination, errorToInt(ErrorCondition::Canceled), nullptr});
  }
  
  arangodb::network::EndpointSpec endpoint;
  Result res = resolveDestination(destination, endpoint);
  if (!res.ok()) { // FIXME return an error  ?!
    return futures::makeFuture(Response{destination, errorToInt(ErrorCondition::Canceled), nullptr});
  }
  TRI_ASSERT(!endpoint.empty());

  auto req = prepareRequest(type, path, std::move(payload), timeout, headers);
  
  // FIXME this is really ugly
  auto promise = std::make_shared<futures::Promise<network::Response>>();
  auto f = promise->getFuture();
  
  ConnectionPool::Ref ref = pool->leaseConnection(endpoint);
  auto conn = ref.connection();
  conn->sendRequest(std::move(req), [destination, ref = std::move(ref),
                                     promise = std::move(promise)](fuerte::Error err,
                                           std::unique_ptr<fuerte::Request> req,
                                           std::unique_ptr<fuerte::Response> res) {
    promise->setValue(network::Response{destination, err, std::move(res)});
  });
  return f;
}
  
template<typename F>
class RequestsState : public std::enable_shared_from_this<RequestsState<F>> {
  
public:
  RequestsState(DestinationId const& destination,
                RestVerb type, std::string const& path,
                velocypack::Buffer<uint8_t>&& payload,
                Timeout timeout, Headers const& headers,
                bool retryNotFound,
                F&& cb)
  : _destination(destination),
  _type(type),
  _path(path),
  _payload(std::move(payload)),
  _headers(headers),
  _startTime(std::chrono::steady_clock::now()),
  _endTime(_startTime + std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout)),
  _retryOnCollNotFound(retryNotFound),
  _cb(std::forward<F>(cb)) {}
  
private:
  
  DestinationId _destination;
  RestVerb _type;
  std::string _path;
  velocypack::Buffer<uint8_t> _payload;
  Headers _headers;
  
  std::chrono::steady_clock::time_point const _startTime;
  std::chrono::steady_clock::time_point const _endTime;
  const bool _retryOnCollNotFound;
  F _cb;
  
  std::unique_ptr<asio_ns::steady_timer> _timer;

public:
  
  // scheduler requests that are due
  void sendRequest() {
    
    auto now = std::chrono::steady_clock::now();
    if (now > _endTime || application_features::ApplicationServer::isStopping()) {
      _cb(Response{_destination, errorToInt(ErrorCondition::Timeout), nullptr});
      return; // we are done
    }
    
//    if (_dueTime <= std::chrono::steady_clock::now()) {
    
    arangodb::network::EndpointSpec endpoint;
    Result res = resolveDestination(_destination, endpoint);
    if (!res.ok()) {
      _cb(Response{_destination, errorToInt(ErrorCondition::Canceled), nullptr});
      return;
    }
    
    ConnectionPool* pool = NetworkFeature::pool();
    if (!pool) {
      LOG_TOPIC(ERR, Logger::FIXME) << "connection pool unavailble";
      _cb(Response{_destination, errorToInt(ErrorCondition::Canceled), nullptr});
      return;
    }
    
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(_endTime - _startTime);
    TRI_ASSERT(timeout.count() > 0);
    
    auto ref = pool->leaseConnection(endpoint);
    auto req = prepareRequest(_type, _path, _payload, timeout, _headers);
    auto self = RequestsState::shared_from_this();
    auto cb = [self, ref, this](fuerte::Error err,
                                std::unique_ptr<fuerte::Request> req,
                                std::unique_ptr<fuerte::Response> res) {
      handleResponse(err, std::move(req), std::move(res));
    };
    ref.connection()->sendRequest(std::move(req), std::move(cb));
  }
  
private:
  
  void handleResponse(fuerte::Error err,
                      std::unique_ptr<fuerte::Request> req,
                      std::unique_ptr<fuerte::Response> res) {
    
    switch (fuerte::intToError(err)) {
      case fuerte::ErrorCondition::NoError:{
        TRI_ASSERT(res);
        if (res->statusCode() == fuerte::StatusOK ||
            res->statusCode() == fuerte::StatusCreated ||
            res->statusCode() == fuerte::StatusAccepted ||
            res->statusCode() == fuerte::StatusNoContent) {
          _cb(Response{_destination, errorToInt(ErrorCondition::NoError), std::move(res)});
          break;
        } else if (res->statusCode() == fuerte::StatusNotFound && _retryOnCollNotFound &&
                   TRI_ERROR_ARANGO_DATA_SOURCE_NOT_FOUND == network::errorCodeFromBody(res->slice())) {
          LOG_TOPIC(DEBUG, Logger::COMMUNICATION) << "retrying later";
        } else {
          _cb(Response{_destination, errorToInt(ErrorCondition::Canceled), std::move(res)});
          break;
        }
        [[clang::fallthrough]]; // intentional
      }
        
      case fuerte::ErrorCondition::CouldNotConnect:
      case fuerte::ErrorCondition::Timeout: {
        // Note that this case includes the refusal of a leader to accept
        // the operation, in which we have to flush ClusterInfo:
        
        auto tryAgainAfter = std::chrono::steady_clock::now() - _startTime;
        if (tryAgainAfter < std::chrono::milliseconds(200)) {
          tryAgainAfter = std::chrono::milliseconds(200);
        } else if (tryAgainAfter > std::chrono::seconds(10)) {
          tryAgainAfter = std::chrono::seconds(10);
        }
        auto dueTime = std::chrono::steady_clock::now() + tryAgainAfter;
        if (dueTime >= _endTime) {
          _cb(Response{_destination, err, std::move(res)});
          break;
        }
        
        if (!_timer) {
          _timer.reset(SchedulerFeature::SCHEDULER->newSteadyTimer());
        }
        
        // TODO what about a shutdown, this will leak ??
        auto self = RequestsState::shared_from_this();
        _timer->expires_at(dueTime);
        _timer->async_wait([self, this] (asio_ns::error_code ec) {
          if (!ec) {
            sendRequest();
          }
        });
        break;
      }
        
      default: // a "proper error" which has to be returned to the client
        _cb(Response{_destination, err, std::move(res)});
        break;
    }
  }
};
  
/// @brief send a request to a given destination, retry until timeout is exceeded
FutureRes sendRequestRetry(DestinationId const& destination, arangodb::fuerte::RestVerb type,
                           std::string const& path, velocypack::Buffer<uint8_t> payload,
                           Timeout timeout, Headers const& headers, bool retryNotFound) {
  PromiseRes p;
  auto f = p.getFuture();
  auto cb = [p = std::move(p)](network::Response&& r) mutable {
    p.setValue(std::move(r));
  };
  //  auto req = prepareRequest(type, path, std::move(payload), timeout, headers);
  auto rs = std::make_shared<RequestsState<decltype(cb)>>(destination, type, path, std::move(payload),
                                                          timeout, headers, retryNotFound, std::move(cb));
  rs->sendRequest();
  
  return f;
}

}} // arangodb::network
