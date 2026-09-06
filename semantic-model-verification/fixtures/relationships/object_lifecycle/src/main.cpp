struct Widget {
    Widget() = default;
    ~Widget() = default;
};
void exercise() {
    auto* value = new Widget{};
    delete value;
}
