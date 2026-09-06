#include "common.hpp"

namespace demo {

static int local_helper(int value) { return value + 1; }

int use_box() {
    Box<int> box{local_helper(4)};
    return box.value;
}

} // namespace demo
