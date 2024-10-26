#pragma once

#include <protocol/types.hpp>

#include <fc/uint128.hpp>

namespace taiyi { namespace chain { namespace util {

    inline u256 to256( const fc::uint128& t )
    {
        u256 v(t.hi);
        v <<= 64;
        v += t.lo;
        return v;
    }

} } } //taiyi::chain::util
