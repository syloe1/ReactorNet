#pragma once

#include "rpc/codec.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace reactornet::rpc {

// The v0.1 handler shape: byte-level on purpose. It keeps the dispatcher
// independent of any serialisation scheme, so protocol, routing and request
// correlation can be built and tested before M5 picks one (#42/#43).
//
// The function receives the request payload and returns the response payload.
using ErasedMethod = std::function<std::string(std::string_view)>;

// Service + method name -> callable.
//
// Thread-safety contract: register everything before the server starts, then
// treat the registry as read-only. freeze() makes the second half explicit and
// is called by RpcServer::start() (#31); dispatch() is const and does not
// mutate anything, so concurrent dispatches need no lock. Mutating after
// freeze() is rejected rather than merely documented because no lock protects
// the maps: a late registration racing a dispatch would be a data race.
//
// Handlers run on the connection's IO thread, synchronously. A slow handler
// stalls every connection on that loop; giving handlers their own thread pool
// is deliberately out of scope (#31 notes, RFC-0002).
class ServiceRegistry {
public:
  // Returns false if the name pair is empty, `fn` is empty, the same
  // service+method is already registered, or the registry is frozen. A
  // duplicate is never overwritten: silent replacement hides a startup bug.
  bool registerMethod(const std::string &service, const std::string &method,
                      ErasedMethod fn);

  // Looks up req.service/req.method and invokes the handler with req.payload.
  // On success sets resp->status = kOk and resp->payload to the handler's
  // return value. On a miss returns false and sets resp->status to
  // kUnknownService or kUnknownMethod (RFC-0001 §4.6), clearing
  // resp->payload. Exceptions from handlers are not caught here; RpcServer's
  // dispatch boundary converts them to INTERNAL_ERROR responses so one faulty
  // handler cannot terminate the loop (#39).
  bool dispatch(const Request &req, Response *resp) const;

  // Number of registered service+method pairs.
  size_t size() const { return methods_.size(); }

  // Seals the registry for the lifetime of the server. Idempotent.
  void freeze() { frozen_ = true; }
  bool frozen() const { return frozen_; }

private:
  // v0.1 keys methods as "Service.Method", as specified by the roadmap. The
  // identifier grammar (RFC-0001 §4.3) forbids '/' and NUL but not '.', so
  // names containing dots could in principle alias; v0.1 accepts that and
  // treats service/method as opaque names.
  static std::string makeKey(const std::string &service,
                             const std::string &method);

  std::unordered_map<std::string, ErasedMethod> methods_;
  std::unordered_set<std::string> services_;
  bool frozen_ = false;
};

} // namespace reactornet::rpc
