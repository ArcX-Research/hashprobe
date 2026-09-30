# Embed the readable tool schemas as short C strings. Requires only POSIX awk.
BEGIN { print "static const char *const schema_lines[] = {" }
{
    printf "    \""
    for (i = 1; i <= length($0); i++) {
        c = substr($0, i, 1)
        if (c == "\\" || c == "\"") printf "\\"
        printf "%s", c
    }
    print "\\n\","
}
END { print "    NULL\n};" }
