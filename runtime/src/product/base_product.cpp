#include "runtime_product.h"

namespace RuntimeProduct {

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor{
        Kind::BaseGame,
        "Metroid: Other M",
    };
    return descriptor;
}

} // namespace RuntimeProduct
