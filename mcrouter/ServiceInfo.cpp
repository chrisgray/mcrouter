/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "ServiceInfo.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <folly/json.h>
#include <folly/Memory.h>
#include <folly/Range.h>

#include "mcrouter/config-impl.h"
#include "mcrouter/config.h"
#include "mcrouter/lib/fbi/cpp/globals.h"
#include "mcrouter/McrouterInstance.h"
#include "mcrouter/options.h"
#include "mcrouter/proxy.h"
#include "mcrouter/ProxyClientCommon.h"
#include "mcrouter/ProxyConfigBuilder.h"
#include "mcrouter/ProxyConfigIf.h"
#include "mcrouter/ProxyMcRequest.h"
#include "mcrouter/ProxyRequestContext.h"
#include "mcrouter/routes/McOpList.h"
#include "mcrouter/routes/ProxyRoute.h"

namespace facebook { namespace memcache { namespace mcrouter {

struct ServiceInfo::ServiceInfoImpl {
  proxy_t* proxy_;
  ProxyRoute& proxyRoute_;
  std::unordered_map<
    std::string,
    std::function<std::string(const std::vector<folly::StringPiece>& args)>>
  commands_;

  ServiceInfoImpl(proxy_t* proxy, const ProxyConfigIf& config);

  void handleRouteCommand(const ProxyMcRequest& req,
                          const std::vector<folly::StringPiece>& args) const;

  template <typename Operation>
  void handleRouteCommandForOp(const ProxyMcRequest& req,
                               std::string keyStr,
                               Operation) const;

  void routeCommandHelper(
    folly::StringPiece op,
    folly::StringPiece key,
    const ProxyMcRequest& req,
    McOpList::Item<0>) const;

  template <int op_id>
  void routeCommandHelper(
    folly::StringPiece op,
    folly::StringPiece key,
    const ProxyMcRequest& req,
    McOpList::Item<op_id>) const;
};

template <typename Operation>
void ServiceInfo::ServiceInfoImpl::handleRouteCommandForOp(
  const ProxyMcRequest& req,
  std::string keyStr,
  Operation) const {

  auto reqCopy = folly::makeMoveWrapper(req.clone());
  auto proxy = proxy_;
  auto proxyRoute = &proxyRoute_;

  proxy_->fiberManager.addTaskFinally(
    [keyStr, proxy, proxyRoute]() {
      auto destinations = folly::make_unique<std::vector<std::string>>();
      auto ctx = std::make_shared<RecordingContext>(
        [&destinations](const ProxyClientCommon& client) {
          destinations->push_back(client.ap.toHostPortString());
        }
      );
      {
        RecordingMcRequest recordingReq(ctx, keyStr);

        /* ignore the reply */
        proxyRoute->route(recordingReq, Operation());
      }
      RecordingContext::waitForRecorded(std::move(ctx));
      return destinations;
    },
    [reqCopy](folly::Try<
              std::unique_ptr<std::vector<std::string>>>&& data) {
      std::string str;
      const auto& destinations = *data;
      for (const auto& d : *destinations) {
        if (!str.empty()) {
          str.push_back('\r');
          str.push_back('\n');
        }
        str.append(d);
      }
      reqCopy->context().sendReply(
        McReply(mc_res_found, str));
    }
  );
}

namespace {
template <class RouteHandle, class Operation>
inline void dumpTree(std::string& tree,
                     int level,
                     const RouteHandle& rh,
                     const RecordingMcRequest& req,
                     Operation) {
  tree.append(std::string(level, ' ') + rh.routeName() + '\n');
  auto targets = rh.couldRouteTo(req, Operation());
  for (auto target : targets) {
    dumpTree(tree, level + 1, *target.get(), req, Operation());
  }
}
}

template <int op_id>
inline std::string routeHandlesCommandHelper(
  folly::StringPiece op,
  const RecordingMcRequest& req,
  const ProxyRoute& proxyRoute,
  McOpList::Item<op_id>) {

  if (op == mc_op_to_string(McOpList::Item<op_id>::op::mc_op)) {
     std::string tree;
     dumpTree(tree, 0, proxyRoute, req, typename McOpList::Item<op_id>::op());
     return tree;
  }

  return routeHandlesCommandHelper(
    op, req, proxyRoute, McOpList::Item<op_id-1>());
}

inline std::string routeHandlesCommandHelper(
  folly::StringPiece op,
  const RecordingMcRequest& req,
  const ProxyRoute& proxyRoute,
  McOpList::Item<0>) {

  throw std::runtime_error("route_handles: unknown op " + op.str());
}

void ServiceInfo::ServiceInfoImpl::routeCommandHelper(
  folly::StringPiece op,
  folly::StringPiece key,
  const ProxyMcRequest& req,
  McOpList::Item<0>) const {

  throw std::runtime_error("route: unknown op " + op.str());
}

template <int op_id>
void ServiceInfo::ServiceInfoImpl::routeCommandHelper(
  folly::StringPiece op,
  folly::StringPiece key,
  const ProxyMcRequest& req,
  McOpList::Item<op_id>) const {

  if (op == mc_op_to_string(McOpList::Item<op_id>::op::mc_op)) {
    handleRouteCommandForOp(req,
                            key.str(),
                            typename McOpList::Item<op_id>::op());
    return;
  }

  routeCommandHelper(op, key, req, McOpList::Item<op_id-1>());
}

/* Must be here since unique_ptr destructor needs to know complete
   ServiceInfoImpl type */
ServiceInfo::~ServiceInfo() {
}

ServiceInfo::ServiceInfo(proxy_t* proxy, const ProxyConfigIf& config)
    : impl_(folly::make_unique<ServiceInfoImpl>(proxy, config)) {
}

ServiceInfo::ServiceInfoImpl::ServiceInfoImpl(proxy_t* proxy,
                                              const ProxyConfigIf& config)
    : proxy_(proxy),
      proxyRoute_(config.proxyRoute()) {

  commands_.emplace("version",
    [] (const std::vector<folly::StringPiece>& args) {
      return MCROUTER_PACKAGE_STRING;
    }
  );

  commands_.emplace("config",
    [this] (const std::vector<folly::StringPiece>& args) {
      if (proxy_->opts.config_str.empty()) {
        return std::string(
          R"({"error": "config is loaded from file and not available"})");
      }
      return std::string(proxy_->opts.config_str);
    }
  );

  commands_.emplace("config_age",
    [proxy] (const std::vector<folly::StringPiece>& args) {
      /* capturing this and accessing proxy_ crashes gcc-4.7 */
      return std::to_string(stat_get_config_age(proxy->stats, time(nullptr)));
    }
  );

  commands_.emplace("config_file",
    [this] (const std::vector<folly::StringPiece>& args) {
      if (proxy_->opts.config_file.empty()) {
        throw std::runtime_error("no config file found!");
      }

      return proxy_->opts.config_file;
    }
  );

  commands_.emplace("options",
    [this] (const std::vector<folly::StringPiece>& args) {
      if (args.size() > 1) {
        throw std::runtime_error("options: 0 or 1 args expected");
      }

      auto optDict = proxy_->opts.toDict();

      if (args.size() == 1) {
        auto it = optDict.find(args[0].str());
        if (it == optDict.end()) {
          throw std::runtime_error("options: option " + args[0].str() +
                                   " not found");
        }
        return it->second;
      }

      // Print all options in order listed in the file
      auto optData = McrouterOptions::getOptionData();
      std::string str;
      for (auto& opt : optData) {
        if (optDict.find(opt.name) != optDict.end()) {
          str.append(opt.name + " " + optDict[opt.name] + "\n");
        }
      }
      return str;
    }
  );

  /*
    This is a special case and handled separately below

  {"route", ...
  },

  */

  commands_.emplace("route_handles",
    [this] (const std::vector<folly::StringPiece>& args) {
      if (args.size() != 2) {
        throw std::runtime_error("route_handles: 2 args expected");
      }
      auto op = args[0];
      auto key = args[1];
      auto ctx = std::make_shared<RecordingContext>(nullptr);
      RecordingMcRequest req(ctx, key.str());

      return routeHandlesCommandHelper(op, req, proxyRoute_,
                                       McOpList::LastItem());
    }
  );

  commands_.emplace("config_md5_digest",
    [&config] (const std::vector<folly::StringPiece>& args) {
      if (config.getConfigMd5Digest().empty()) {
        throw std::runtime_error("no config md5 digest found!");
      }
      return config.getConfigMd5Digest();
    }
  );

  commands_.emplace("config_sources_info",
    [this] (const std::vector<folly::StringPiece>& args) {
      auto configInfo = proxy_->router->configApi().getConfigSourcesInfo();
      return folly::toPrettyJson(configInfo).toStdString();
    }
  );

  commands_.emplace("preprocessed_config",
    [this] (const std::vector<folly::StringPiece>& args) {
      std::string confFile;
      if (!proxy_->router->configApi().getConfigFile(confFile)) {
        throw std::runtime_error("can not load config");
      }
      ProxyConfigBuilder builder(proxy_->opts,
                                 &proxy_->router->configApi(),
                                 confFile);
      folly::json::serialization_opts jsonOpts;
      jsonOpts.pretty_formatting = true;
      jsonOpts.sort_keys = true;
      return folly::json::serialize(builder.preprocessedConfig(),
                                    jsonOpts).toStdString();
    }
  );

  commands_.emplace("hostid",
    [] (const std::vector<folly::StringPiece>& args) {
      return folly::to<std::string>(globals::hostid());
    }
  );
}

void ServiceInfo::ServiceInfoImpl::handleRouteCommand(
  const ProxyMcRequest& req,
  const std::vector<folly::StringPiece>& args) const {

  if (args.size() != 2) {
    throw std::runtime_error("route: 2 args expected");
  }
  auto op = args[0];
  auto key = args[1];

  routeCommandHelper(op, key, req, McOpList::LastItem());
}

void ServiceInfo::handleRequest(const ProxyMcRequest& req) const {
  auto key = req.keyWithoutRoute();
  auto p = key.find('(');
  auto cmd = key;
  folly::StringPiece argsStr(key.end(), key.end());
  if (p != folly::StringPiece::npos &&
      key.back() == ')') {
    assert(key.size() - p >= 2);
    cmd = folly::StringPiece(key.begin(), key.begin() + p);
    argsStr = folly::StringPiece(key.begin() + p + 1,
                                 key.begin() + key.size() - 1);
  }
  std::vector<folly::StringPiece> args;
  if (!argsStr.empty()) {
    folly::split(',', argsStr, args);
  }

  std::string replyStr;
  try {
    if (cmd == "route") {
      /* Route is a special case since it involves background requests */
      impl_->handleRouteCommand(req, args);
      return;
    }

    auto it = impl_->commands_.find(cmd.str());
    if (it == impl_->commands_.end()) {
      throw std::runtime_error("unknown command: " + cmd.str());
    }
    replyStr = it->second(args);
    if (!replyStr.empty() && replyStr.back() == '\n') {
      replyStr = replyStr.substr(0, replyStr.size() - 1);
    }
  } catch (const std::exception& e) {
    replyStr = std::string("ERROR: ") + e.what();
  }

  req.context().sendReply(
    McReply(mc_res_found, replyStr));
}

}}}  // facebook::memcache::mcrouter
