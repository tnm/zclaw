# zclaw docs-site

Custom static docs site with a print-book visual style inspired by classic C references.

## Preview locally

```bash
./scripts/docs-site.sh
# or:
./scripts/docs-site.sh --host 0.0.0.0 --port 8788 --open
```

## Structure

- `index.html` - overview and chapter map
- `getting-started.html` - setup, flash, provision flow
- `tools.html` - tool reference and schedule grammar
- `architecture.html` - runtime/task model
- `security.html` - security and ops
- `styles.css` - visual system and responsive layout
- `app.js` - sidebar/nav behavior
