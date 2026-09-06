struct Base { virtual void draw(); };
struct Derived : Base { void draw() override; };
