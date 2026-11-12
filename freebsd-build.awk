# Enable the existing dashboard data-gathering block on FreeBSD while leaving
# the Linux and other-Unix paths untouched. This generates a temporary C source.
/^#else \/\* !__linux__ — clock-only mode \*\// {
    print "#elif defined(__FreeBSD__)"
    print "#include \"freebsd-stats.inc\""
    print
    next
}

/^static void draw_frame\(int frame\)/ { in_frame = 1 }
in_frame && /^#ifdef __linux__$/ {
    sub(/^#ifdef __linux__$/, "#if defined(__linux__) || defined(__FreeBSD__)")
}

{
    gsub(/cpu stats need \/proc \(Linux\)/, "CPU statistics unavailable")
    gsub(/memory stats need \/proc \(Linux\)/, "Memory statistics unavailable")
    print
}

in_frame && /^}$/ { in_frame = 0 }
