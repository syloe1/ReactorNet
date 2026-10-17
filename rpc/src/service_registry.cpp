#include "rpc/service_registry.h"

#include <utility>

namespace reactornet::rpc {

std::string ServiceRegistry::makeKey(const std::string &service,
                                     const std::string &method) {
  std::string key;
  key.reserve(service.size() + 1 + method.size());
  key += service;
  key += '.';
  key += method;
  return key;
}

bool ServiceRegistry::registerMethod(const std::string &service,
                                     const std::string &method,
                                     ErasedMethod fn) {
  if (frozen_ || service.empty() || method.empty() || !fn) {
    return false;
  }

  // emplace() refuses to replace an existing entry; insert() on the service
  // set runs only after the pair was actually added.
  auto result = methods_.emplace(makeKey(service, method), std::move(fn));
  if (!result.second) {
    return false;
  }
  services_.insert(service);
  return true;
}

bool ServiceRegistry::dispatch(const Request &req, Response *resp) const {
  if (resp == nullptr) {
    return false;
  }

  const auto method = methods_.find(makeKey(req.service, req.method));
  if (method == methods_.end()) {
    resp->status = services_.count(req.service) != 0
                       ? StatusCode::kUnknownMethod
                       : StatusCode::kUnknownService;
    resp->payload.clear();
    return false;
  }

  resp->status = StatusCode::kOk;
  resp->payload = method->second(req.payload);
  return true;
}

} // namespace reactornet::rpc
