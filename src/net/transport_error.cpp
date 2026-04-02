#include <wuwe/net/transport_error.h>

#include <cpr/error.h>

#include <system_error>

WUWE_NAMESPACE_BEGIN

namespace {

template<typename T>
struct constant_init {
    union {
        T obj;
    };
    constexpr constant_init() noexcept : obj() {}

    ~constant_init() { /* do nothing, union object is not destroyed */}
};

struct transport_error_category_impl final : public std::error_category {
public:
    const char* name() const noexcept final {
        return "transport";
    }

    std::string message(int code) const final {
        return std::to_string(static_cast<cpr::ErrorCode>(code));
    }
};

} // namespace

const std::error_category& transport_error_category() noexcept {
    static constant_init<transport_error_category_impl> transport_error_category_instance;
    return transport_error_category_instance.obj;
}

WUWE_NAMESPACE_END
