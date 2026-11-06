#include "core.hpp"
// Archive extraction exists only in the background worker; bootstrap re-verifies
// its signed inventory, archive hash and every extracted byte without ZIP I/O.
namespace wip {void extract(const fs::path&,const fs::path&,const Json&){throw std::runtime_error("bootstrap has no archive extraction");}}
