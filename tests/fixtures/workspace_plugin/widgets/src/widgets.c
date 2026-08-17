/* NEGATIVE CONTROL 1's other half. `render` is defined here, once, at top
 * level, in a member of the same workspace — and `host` calls a function of
 * that name. Nothing in `host` includes anything from this repository, so the
 * crossing was never declared and the two must not be joined. */
int render(void) {
    return 1;
}

int widget_count(void) {
    return render();
}
