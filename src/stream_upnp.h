#pragma once
#include <cstdint>
#include <string>

namespace openshao::stream::upnp {

// Starts a background thread: discover (2000 ms timeout), find a valid IGD, map UDP port->port on
// the local LAN address, description "OpenShao", lease 3600 s, renewed every 30 min.
void Start(uint16_t port);

// Removes the mapping (if any) and joins the thread. Bounded: must return within ~3 s.
void Stop();

// "a.b.c.d:port" once the mapping succeeded, else empty.
std::string ExternalEndpoint();

} // namespace openshao::stream::upnp
