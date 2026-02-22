#!/usr/bin/env python3
"""Run zclaw tool-calling API harness against Anthropic."""

import argparse
import os
import sys

from provider_harness import PROVIDERS, interactive_mode, run_conversation


def main() -> None:
    provider = PROVIDERS["anthropic"]
    parser = argparse.ArgumentParser(description="Test zclaw tool calling with Anthropic API")
    parser.add_argument("message", nargs="?", help="Message to send")
    parser.add_argument("--interactive", "-i", action="store_true", help="Interactive mode")
    parser.add_argument("--quiet", "-q", action="store_true", help="Only show final response")
    parser.add_argument(
        "--model",
        "-m",
        help=f"Model to use (default: {provider.model_env} env or {provider.default_model})",
    )
    args = parser.parse_args()

    api_key = os.environ.get(provider.api_key_env)
    if not api_key:
        print(f"Error: {provider.api_key_env} environment variable not set")
        sys.exit(1)

    model = args.model or os.environ.get(provider.model_env, provider.default_model)

    if args.interactive:
        interactive_mode(provider, api_key, model)
    elif args.message:
        run_conversation(provider, args.message, api_key, model, user_tools=[], verbose=not args.quiet)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
