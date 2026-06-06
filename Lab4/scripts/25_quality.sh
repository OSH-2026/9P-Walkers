#!/usr/bin/env bash
# 25_quality.sh - report section 2.5 prompt quality evaluation.

set -euo pipefail

tag=A
temp=0.7
n_predict=256
model_override=''
prompt_file=''

usage() {
    cat <<'EOF'
Usage:
  ./scripts/25_quality.sh [--tag A] [--temp 0.7] [--n-predict 256]
                         [--model PATH] [--prompt-file PATH]

Compatibility aliases:
  -Tag, -Temp, -NPredict, -Model, -PromptFile
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag|-Tag)
            tag=$2
            shift 2
            ;;
        --temp|-Temp)
            temp=$2
            shift 2
            ;;
        --n-predict|-NPredict)
            n_predict=$2
            shift 2
            ;;
        --model|-Model)
            model_override=$2
            shift 2
            ;;
        --prompt-file|-PromptFile)
            prompt_file=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"
print_common_summary

if [[ -n "$model_override" ]]; then
    model="$(cd "$(dirname "$model_override")" && pwd)/$(basename "$model_override")"
fi
if [[ -z "$prompt_file" ]]; then
    prompt_file="$scripts_dir/prompts_5.json"
fi

command -v python3 >/dev/null 2>&1 || die "25_quality.sh 需要 python3 解析 prompts_5.json"
assert_file "$model"
assert_file "$prompt_file"
assert_file "$llama_cli"

stamp="$(date +%Y%m%d_%H%M%S)"
out_dir="$results_dir/25_quality_${tag}_$stamp"
mkdir -p "$out_dir"

printf '%s配置[%s]  model=%s  temp=%s  n=%s%s\n' \
    "$color_cyan" "$tag" "$(basename "$model")" "$temp" "$n_predict" "$color_reset"

while IFS= read -r -d '' id \
    && IFS= read -r -d '' category \
    && IFS= read -r -d '' prompt; do
    printf '\n%s--- #%s [%s] ---%s\n' "$color_yellow" "$id" "$category" "$color_reset"
    pf="$(new_prompt_file "$prompt")"
    invoke_with_peak_memory "$llama_cli" \
        -- -m "$model" -f "$pf" -n "$n_predict" \
        --temp "$temp" --threads 8 -st --no-display-prompt
    rm -f "$pf"

    if [[ "$INVOKE_EXIT_CODE" -ne 0 ]]; then
        warn "llama-cli exit code=$INVOKE_EXIT_CODE for prompt #$id"
    fi

    file="$out_dir/p${id}_${category}.txt"
    {
        printf '# Prompt %s [%s]  config=%s temp=%s model=%s\n' \
            "$id" "$category" "$tag" "$temp" "$(basename "$model")"
        printf '## PROMPT\n%s\n\n' "$prompt"
        printf '## OUTPUT\n%s\n' "$INVOKE_STDOUT"
    } >"$file"
    printf '  -> %s\n' "$file"
done < <(python3 - "$prompt_file" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    prompts = json.load(f)

for item in prompts:
    for value in (item["id"], item["category"], item["prompt"]):
        sys.stdout.write(str(value))
        sys.stdout.write("\0")
PY
)

printf '\n%s完成。请人工打分(正确性/完整性/流畅性, 1~5)，输出在: %s%s\n' \
    "$color_green" "$out_dir" "$color_reset"
