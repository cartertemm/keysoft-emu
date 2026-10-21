# CLAUDE.md

## Setup facts

- The QEMU configure step needs `diff`. On a fresh MSYS2 install (such as the CI runner), install the `diffutils` package, or meson setup fails with "Program 'diff' not found".
