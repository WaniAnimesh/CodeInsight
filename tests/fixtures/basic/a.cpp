#include "common.hpp"

namespace demo {

static int local_helper(int value) { return value * CODEINSIGHT_SCALE; }
static int intentionally_unused() { return 99; }

int Derived::draw(Index value) const { return local_helper(static_cast<int>(value)); }

int process(Base& object, Index value) { return object.draw(value); }

int overload(int value) { return header_local(value); }
int overload(double value) { return static_cast<int>(value); }

int tracked_state{};
Tracked::Tracked() = default;
Tracked::~Tracked() = default;

void exercise_expression_relationships() {
    int local = tracked_state;
    tracked_state = local;
    ++tracked_state;
    auto callback = &process;
    (void)callback;
    auto* tracked = new Tracked{};
    delete tracked;
}

int exported_api() { return tracked_state; }

} // namespace demo
