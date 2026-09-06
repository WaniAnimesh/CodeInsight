struct Base { virtual int draw() const = 0; };
struct Derived final : Base { int draw() const override { return 1; } };
int invoke(Base& value) { return value.draw(); }
