# zclaw docs-site

Custom static docs site with a print-book visual style inspired by classic C references.

## Preview locally

```bash
./scripts/docs-site.sh
# or:
./scripts/docs-site.sh --host 0.0.0.0 --port 8788 --open
```

## Structure

- `README.html` - web-formatted README landing page
- `index.html` - overview and chapter map
- `getting-started.html` - setup, flash, provision flow
- `tools.html` - tool reference and schedule grammar
- `architecture.html` - runtime/task model
- `security.html` - security and ops
- `build-your-own-tool.html` - custom tool design and maintenance workflow
- `local-dev.html` - local dev, provisioning profiles, and hacking loops
- `use-cases.html` - useful and playful scenarios for on-device assistants
- `changelog.html` - release history and upgrade notes
- `styles.css` - visual system and responsive layout
- `app.js` - sidebar/nav behavior
