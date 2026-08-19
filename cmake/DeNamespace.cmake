# Rewrites the C keyword-safe-but-C++-reserved identifier `namespace` in a
# wayland-scanner generated header so the header can be included from C++.
file(READ "${FILE}" contents)
string(REGEX REPLACE "([^_a-zA-Z0-9])namespace([^_a-zA-Z0-9])" "\\1wl_namespace\\2" out "${contents}")
file(WRITE "${FILE}" "${out}")
