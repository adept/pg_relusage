#!/bin/bash
# Reformat C sources with PostgreSQL's pgindent, inside the cached test image
# (see run_test.sh), so that nothing has to be installed locally.
#
# Usage: tools/pgindent.sh [--check | --diff] [file.c ...]
#
# Without files, all *.c files in the repository root are processed.
#   --check  do not modify files, exit with non-zero status if any of them need reformatting
#   --diff   do not modify files, show the changes pgindent would make
#
# Use /* */ comments only: pgindent does not convert // comments, and mangles them.
#
# pgindent and pg_bsd_indent sources are fetched from the PostgreSQL repository
# into .pgindent/ (git-ignored) on the first run. Set PGVERSION to use the
# pgindent of a different PostgreSQL version (default: 18).
set -euo pipefail

cd "$(dirname "$0")/.."

ver="${PGVERSION:-18}"
branch="REL_${ver}_STABLE"
image="pg_relusage-test:${ver}"
src=".pgindent/${branch}"

mode=()
files=()
for arg in "$@"; do
    case "${arg}" in
        --check|--diff) mode+=("${arg}") ;;
        -*) echo "Unknown option: ${arg}" >&2; exit 2 ;;
        *) files+=("${arg}") ;;
    esac
done
[ ${#files[@]} -eq 0 ] && files=(*.c)

if [ ! -d "${src}" ]; then
    git clone --quiet --depth 1 --filter=blob:none --sparse --branch "${branch}" \
        https://github.com/postgres/postgres.git "${src}"
    git -C "${src}" sparse-checkout set src/tools/pgindent src/tools/pg_bsd_indent
fi

docker build --quiet --build-arg PGVERSION="${ver}" -t "${image}" test/ >/dev/null

# Typedefs defined in our own sources, which pgindent needs to know about to
# format declarations that use them (in addition to PostgreSQL's typedefs.list)
typedefs=$(sed -n 's/^}[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\);$/\1/p' "${files[@]}" | sort -u | tr '\n' ' ')

docker run --rm -e AS_USER=worker -e LOCAL_UID="$(id -u)" \
    --volume "$PWD:/repo" "${image}" bash -c '
      set -eu
      cd /repo
      src="$1"; typedefs="$2"; shift 2
      if [ ! -x "${src}/bin/pg_bsd_indent" ]; then
        mkdir -p "${src}/bin"
        (cd "${src}/src/tools/pg_bsd_indent" &&
         gcc -O2 -I. -I"$(pg_config --includedir-server)" *.c \
             -L"$(pg_config --pkglibdir)" -lpgport -o "/repo/${src}/bin/pg_bsd_indent")
      fi
      "${src}/src/tools/pgindent/pgindent" \
        --indent="${src}/bin/pg_bsd_indent" \
        --typedefs="${src}/src/tools/pgindent/typedefs.list" \
        --list-of-typedefs="${typedefs}" "$@"
    ' bash "${src}" "${typedefs}" ${mode[@]+"${mode[@]}"} "${files[@]}"
