// Stub helper for spawn fail-fast tests. Exits immediately so the parent must
// report the child's exit code instead of waiting 15 s on ConnectNamedPipe.
int main()
{
    return 42;
}
