#!/bin/sh
# AppAlchemist entry point.
#
# The build produces two executables: "appalchemist-gui" (GTK4/libadwaita
# interface) and "appalchemist-cli" (headless converter). "appalchemist" is the
# product's command name and is what desktop entries, MIME handlers and the
# documentation invoke, so it dispatches to whichever binary fits the request:
# a conversion on the command line runs headless, everything else opens the
# interface.

set -eu

BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
GUI="${BIN_DIR}/appalchemist-gui"
CLI="${BIN_DIR}/appalchemist-cli"

TARGET="${GUI}"
case "${1:-}" in
    --convert|-c|--batch|-b|--json|--dry-run|--quiet|-q|--output|-o|--no-launch|--version|-v|--help|-h)
        TARGET="${CLI}"
        ;;
    *.deb|*.rpm|*.tar|*.tar.gz|*.tgz|*.tar.xz|*.tar.bz2|*.tar.zst|*.zip)
        TARGET="${CLI}"
        ;;
esac

# Fall back to whichever binary this installation actually shipped.
if [ ! -x "${TARGET}" ]; then
    if [ "${TARGET}" = "${CLI}" ] && [ -x "${GUI}" ]; then
        TARGET="${GUI}"
    elif [ -x "${CLI}" ]; then
        TARGET="${CLI}"
    else
        echo "appalchemist: neither appalchemist-gui nor appalchemist-cli is installed in ${BIN_DIR}" >&2
        exit 1
    fi
fi

exec "${TARGET}" "$@"
