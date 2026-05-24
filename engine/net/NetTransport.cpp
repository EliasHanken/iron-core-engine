#include "net/NetTransport.h"

namespace iron {

// Out-of-line virtual destructor anchor: keeps the vtable in this
// translation unit instead of every consumer. Body is empty by design.
NetTransport::~NetTransport() = default;

} // namespace iron
