#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
repo_root=${script_dir:h:h}
root=${MONADO_WINE_DXMT_ROOT:-${repo_root}/build-wine-dxmt}
wine=${root}/bin/wine-dxmt
producer=${root}/bin/macos_wine_d3d11_iosurface_producer.exe
probe=${MONADO_IOSURFACE_IMPORT_PROBE:-${repo_root}/build/tests/tests_macos_iosurface_import_probe}

if [[ ! -x "${wine}" ]]; then
	print -u2 "Missing pinned Wine/DXMT stack. Run:"
	print -u2 "  scripts/macos/provision-wine-dxmt.zsh"
	exit 1
fi

if [[ ! -f "${producer}" ]]; then
	"${script_dir}/build-wine-d3d11-iosurface-producer.zsh" >/dev/null
fi

if [[ ! -x "${probe}" ]]; then
	for candidate in \
		"${repo_root}/build-release/tests/tests_macos_iosurface_import_probe" \
		"${repo_root}/build-service/tests/tests_macos_iosurface_import_probe"
	do
		if [[ -x "${candidate}" ]]; then
			probe=${candidate}
			break
		fi
	done
fi

if [[ ! -x "${probe}" ]]; then
	print -u2 "Could not find tests_macos_iosurface_import_probe."
	print -u2 "Set MONADO_IOSURFACE_IMPORT_PROBE to its path or build this Monado branch first."
	exit 1
fi

run_dir=${root}/run
mkdir -p "${run_dir}"

done_file=${run_dir}/d3d11-iosurface-done.$$
ids_file=${run_dir}/d3d11-iosurface-ids.$$
stderr_file=${run_dir}/d3d11-iosurface-producer.$$.log

rm -f "${done_file}" "${ids_file}" "${stderr_file}"

# Wine's Z: drive maps to the macOS root filesystem.
windows_done="Z:${done_file//\//\\}"

cleanup()
{
	touch "${done_file}" 2>/dev/null || true

	if [[ -n ${producer_pid:-} ]]; then
		wait "${producer_pid}" 2>/dev/null || true
	fi

	rm -f "${done_file}" "${ids_file}" "${stderr_file}"
}
trap cleanup EXIT INT TERM

MONADO_IOSURFACE_DONE_FILE="${windows_done}" \
	"${wine}" "${producer}" \
	>"${ids_file}" \
	2>"${stderr_file}" &
producer_pid=$!

# Wait until the producer has emitted a complete newline-terminated ID line.
for _ in {1..200}; do
	if [[ -s "${ids_file}" ]] && grep -q $'\n' "${ids_file}"; then
		break
	fi

	if ! kill -0 "${producer_pid}" 2>/dev/null; then
		print -u2 "D3D11 producer exited before publishing IOSurface IDs:"
		cat "${stderr_file}" >&2 || true
		exit 1
	fi

	sleep 0.1
done

if [[ ! -s "${ids_file}" ]] || ! grep -q $'\n' "${ids_file}"; then
	print -u2 "Timed out waiting for D3D11 producer IOSurface IDs."
	cat "${stderr_file}" >&2 || true
	exit 1
fi

IFS=' ' read -r id0 id1 id2 extra < "${ids_file}" || true

# Wine/Windows stdout may be CRLF. Strip a trailing CR from every field.
id0=${id0%$'\r'}
id1=${id1%$'\r'}
id2=${id2%$'\r'}
extra=${extra%$'\r'}

if [[ -z "${id0}" || -z "${id1}" || -z "${id2}" || -n "${extra}" ]]; then
	print -u2 "D3D11 producer did not publish exactly three IOSurface IDs."
	cat "${ids_file}" >&2 || true
	cat "${stderr_file}" >&2 || true
	exit 1
fi

for id in "${id0}" "${id1}" "${id2}"; do
	case "${id}" in
		''|*[!0-9]*)
			print -u2 "Invalid IOSurface ID from D3D11 producer: ${id}"
			exit 1
			;;
	esac

	if [[ "${id}" == "0" ]]; then
		print -u2 "Invalid IOSurface ID from D3D11 producer: ${id}"
		exit 1
	fi
done

print "Wine/DXMT D3D11 producer published IOSurface IDs: ${id0} ${id1} ${id2}"

"${probe}" "${id0}" "${id1}" "${id2}"

touch "${done_file}"

if ! wait "${producer_pid}"; then
	print -u2 "D3D11 producer reported failure:"
	cat "${stderr_file}" >&2 || true
	exit 1
fi
producer_pid=""

cat "${stderr_file}"
print "PASS: Windows D3D11 -> Wine/DXMT -> IOSurface -> Monado swapchain import"
