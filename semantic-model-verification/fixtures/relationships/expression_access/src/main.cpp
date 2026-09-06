int state = 0;
void callback() {}
void exercise() {
    int local = state;
    state = local;
    ++state;
    auto pointer = &callback;
    pointer();
}
