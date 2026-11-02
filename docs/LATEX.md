# LaTeX in MD Studio

MD Studio typesets LaTeX math itself (no TeX installation needed). It uses a small TeX-style engine
(`src/MathRenderer.cpp`) with the embedded **Latin Modern Math** font and the layout rules from the
TeXbook, driven by the font's own OpenType MATH table.

## Where formulas are recognised

| Syntax | Meaning |
|--------|---------|
| `$ ... $` | inline formula |
| `\( ... \)` | inline formula |
| `$$ ... $$` | display formula (own line(s) => centred block; inside a sentence => display style inline) |
| `\[ ... \]` | display formula |
| ```` ```math ```` fenced block | display formula (GitHub style) |

Rules that avoid false positives: `$5 and $10` is plain text (the opening `$` must be followed by a
non-space, the closing one preceded by a non-space and not followed by a letter/digit), `\$` is a literal
dollar, and code spans / fenced code blocks are never touched.

## Supported commands (highlights)

* **Structure**: `\frac \dfrac \tfrac \binom \sqrt[n]{} ^ _ '` primes, `\left \right \middle`, `\big \Big \bigg \Bigg`
* **Big operators**: `\sum \prod \coprod \int \iint \iiint \oint \bigcup \bigcap \bigoplus ...` with `\limits` / `\nolimits`,
  `\lim \max \min \sup \inf \det \gcd` and all standard functions (`\sin \cos \log \ln \exp ...`), `\operatorname{}`
* **Symbols**: full Greek alphabet (+ variants), ~250 operators / relations / arrows / misc symbols, `\not`
* **Fonts**: `\mathbb \mathcal \mathfrak \mathbf \mathit \mathrm \mathsf \mathtt \boldsymbol \bm \text \textbf`
* **Accents & lines**: `\hat \widehat \tilde \widetilde \bar \vec \dot \ddot \check \breve \acute \grave \overline \underline \overbrace \underbrace`
* **Stacking / arrows**: `\overset \underset \stackrel \substack \xrightarrow[below]{above}`
* **Environments**: `matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix smallmatrix cases array aligned align gather split`
* **Spacing & misc**: `\, \: \; \! \quad \qquad \hspace{} \phantom \boxed \color{} \textcolor{}{} \displaystyle \textstyle \pmod \bmod`

Unknown commands are shown in red so typos are easy to spot.

## Editing help

* Type `\` + letters for command completion (ghost text and drop-down). `Tab` accepts; templates such as
  `\frac{}{}` put the caret in the first slot.
* Snippets: `math`, `imath`, `equation`, `matrix`, `cases`, `align`.
* Format menu: **Inline Math** (Ctrl+M), **Math Block** (Ctrl+Shift+M).
* Math is coloured in the editor, including multi-line `$$` blocks.

## Settings (Settings > Preview)

Render formulas on/off, formula size (% of the text size), plus all preview font/spacing/page options.

## Export

* **HTML**: formulas are embedded as images with the LaTeX source as `alt` text (dark-mode aware).
* **PDF / ODT**: formulas are placed inline, baseline-aligned.
* **DOCX**: formulas are embedded as images (lowered to the text baseline; Word honours this, LibreOffice
  shows them a few pixels high). For **native, editable Word equations** use *File > Export As > More formats
  via Pandoc > Word document* - Pandoc receives the raw LaTeX.

## Regenerating the font data

`src/MathGlyphData.h` is generated from the font's MATH table (size variants, glyph assemblies, italic
corrections): `python3 tools/gen_math_glyphs.py resources/fonts/latinmodern-math.otf > src/MathGlyphData.h`
(needs `pip install fonttools`). Latin Modern Math is distributed under the GUST Font License
(`resources/fonts/GUST-FONT-LICENSE.txt`).
