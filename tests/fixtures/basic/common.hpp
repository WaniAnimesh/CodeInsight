#pragma once

#define CODEINSIGHT_SCALE 2

namespace demo {

static int header_local(int value) { return value; }

using Index = unsigned long;

enum class Color { Red, Green };

struct Base {
    virtual ~Base() = default;
    virtual int draw(Index value) const = 0;
};

struct Derived final : Base {
    int draw(Index value) const override;
};

struct Tracked {
    Tracked();
    ~Tracked();
    int value{};
};

template<class T> struct Box { T value; };
template<> struct Box<bool> { bool value; };

int process(Base& object, Index value);
int overload(int value);
int overload(double value);
extern int tracked_state;
void exercise_expression_relationships();
__declspec(dllexport) int exported_api();

} // namespace demo
