/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include <folly/Conv.h>
#include <folly/dynamic.h>
#include <folly/Optional.h>

#include "mcrouter/lib/config/RouteHandleFactory.h"
#include "mcrouter/lib/fbi/cpp/util.h"
#include "mcrouter/lib/mc/msg.h"
#include "mcrouter/lib/mc/protocol.h"
#include "mcrouter/lib/Operation.h"
#include "mcrouter/lib/Reply.h"
#include "mcrouter/RoutingPrefix.h"

namespace facebook { namespace memcache { namespace mcrouter {

/**
 * Modifies key of current request.
 *  set_routing_prefix if present, routing prefix of a key will be set to this
 *                     value
 *  ensure_key_prefix if key doesn't start with this value, it will be appended
 *                    to the key
 *
 * Example:
 *  ModifyKeyRoute
 *    set_routing_prefix = "/a/b/"
 *    ensure_key_prefix = "foo"
 * "/a/b/a" => "/a/b/fooa"
 * "foo" => "/a/b/foo"
 * "/b/c/o" => "/a/b/fooo"
 */
template <class RouteHandleIf>
class ModifyKeyRoute {
 public:
  static std::string routeName() { return "modify-key"; }

  ModifyKeyRoute(RouteHandleFactory<RouteHandleIf>& factory,
                 const folly::dynamic& json) {
    auto jtarget = json.get_ptr("target");
    checkLogic(jtarget, "ModifyKeyRoute: no target");
    target_ = factory.create(*jtarget);

    if (auto jroutingPrefix = json.get_ptr("set_routing_prefix")) {
      auto rp = jroutingPrefix->stringPiece();
      if (rp.empty()) {
        routingPrefix_ = "";
      } else {
        try {
          routingPrefix_ = RoutingPrefix(rp).str();
        } catch (const std::exception& e) {
          throw std::logic_error("ModifyKeyRoute: set_routing_prefix: " +
                                 std::string(e.what()));
        }
      }
    }
    if (auto jkeyPrefix = json.get_ptr("ensure_key_prefix")) {
      keyPrefix_ = jkeyPrefix->stringPiece().str();
      auto err = mc_client_req_key_check(to<nstring_t>(keyPrefix_));
      checkLogic(keyPrefix_.empty() || err == mc_req_err_valid,
                 "ModifyKeyRoute: invalid key prefix '{}', {}", keyPrefix_,
                 mc_req_err_to_string(err));
    }
  }

  template <class Operation, class Request>
  std::vector<std::shared_ptr<RouteHandleIf>>
  couldRouteTo(const Request& req, Operation) const {
    return { target_ };
  }

  template <class Operation, class Request>
  typename ReplyType<Operation, Request>::type
  route(const Request& req, Operation) const {
    folly::StringPiece rp = routingPrefix_.hasValue()
      ? routingPrefix_.value()
      : req.routingPrefix();

    if (!req.keyWithoutRoute().startsWith(keyPrefix_)) {
      auto key = folly::to<std::string>(rp, keyPrefix_, req.keyWithoutRoute());
      return routeReqWithKey(req, key, Operation());
    } else if (routingPrefix_.hasValue() && rp != req.routingPrefix()) {
      auto key = folly::to<std::string>(rp, req.keyWithoutRoute());
      return routeReqWithKey(req, key, Operation());
    }
    return target_->route(req, Operation());
  }

 private:
  std::shared_ptr<RouteHandleIf> target_;
  folly::Optional<std::string> routingPrefix_;
  std::string keyPrefix_;

  template <class Operation, class Request>
  typename ReplyType<Operation, Request>::type
  routeReqWithKey(const Request& req, folly::StringPiece key, Operation) const {
    typedef typename ReplyType<Operation, Request>::type Reply;

    auto err = mc_client_req_key_check(to<nstring_t>(key));
    if (err != mc_req_err_valid) {
      return Reply(ErrorReply, "ModifyKeyRoute: invalid key: " +
          std::string(mc_req_err_to_string(err)));
    }
    auto cloneReq = req.clone();
    cloneReq.setKey(key);
    return target_->route(cloneReq, Operation());
  }
};

}}}  // facebook::memcache::mcrouter
