// =============================================================================
//  MathRenderer.cpp
//
//  Sections
//    1. Font access (embedded Latin Modern Math through QRawFont) + MATH data
//    2. Parse tree + LaTeX parser
//    3. Layout engine (TeXbook Appendix G rules, using the font's MATH constants)
//    4. Public API: render(), extractMath(), applyMath()
//
//  Coordinate convention: layout is done in FONT UNITS (1 em = 1000) with y
//  pointing DOWN like Qt.  A Box describes ink extents relative to its own
//  baseline; draw(painter, x, y) paints it with (x, y) = left end of the baseline.
//  The painter is scaled once at the end (units -> device pixels).
// =============================================================================
#include "MathRenderer.h"
#include "MathGlyphData.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace {

enum Cls { Ord = 0, Op, Bin, Rel, Open, Close, Punct, Inner };

// =============================================================================
//  1. Font access
// =============================================================================
class MathFont {
public:
    MathFont()
    {
        QFile f(":/fonts/latinmodern-math.otf");
        if (f.open(QIODevice::ReadOnly)) m_data = f.readAll();
        m_raw = QRawFont(m_data, 1000.0, QFont::PreferNoHinting);   // 1 px == 1 font unit
    }
    bool ok() const { return m_raw.isValid(); }

    quint32 glyph(uint cp)
    {
        auto it = m_cp.find(cp);
        if (it != m_cp.end()) return it.value();
        const char32_t c32 = cp;
        const QList<quint32> g = m_raw.glyphIndexesForString(QString::fromUcs4(&c32, 1));
        const quint32 gi = g.isEmpty() ? 0 : g[0];
        m_cp.insert(cp, gi);
        return gi;
    }
    qreal advance(quint32 g)
    {
        auto it = m_adv.find(g);
        if (it != m_adv.end()) return it.value();
        const auto a = m_raw.advancesForGlyphIndexes({g}, QRawFont::UseDesignMetrics);
        const qreal v = a.isEmpty() ? 0 : a[0].x();
        m_adv.insert(g, v);
        return v;
    }
    QRectF ink(quint32 g)   // ink rectangle, y DOWN, relative to the glyph origin
    {
        auto it = m_ink.find(g);
        if (it != m_ink.end()) return it.value();
        const QRectF r = m_raw.boundingRect(g);
        m_ink.insert(g, r);
        return r;
    }
    const QPainterPath &path(quint32 g)
    {
        auto it = m_path.find(g);
        if (it != m_path.end()) return it.value();
        return m_path.insert(g, m_raw.pathForGlyph(g)).value();
    }
private:
    QByteArray m_data;
    QRawFont   m_raw;
    QHash<uint, quint32> m_cp;
    QHash<quint32, qreal> m_adv;
    QHash<quint32, QRectF> m_ink;
    QHash<quint32, QPainterPath> m_path;
};

MathFont &F() { static MathFont f; return f; }

// ---- MATH table lookups (generated data) --------------------------------------
const MGConstruction *findConstruction(const MGConstruction *arr, int n, uint cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (arr[mid].cp == cp) return &arr[mid];
        if (arr[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}
const MGConstruction *vertConstruction(uint cp)  { return findConstruction(kVertConstructions,  kVertConstructionCount,  cp); }
const MGConstruction *horizConstruction(uint cp) { return findConstruction(kHorizConstructions, kHorizConstructionCount, cp); }

bool lookupValue(const MGValue *arr, int n, quint32 glyph, int *out)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (arr[mid].glyph == glyph) { *out = arr[mid].value; return true; }
        if (arr[mid].glyph < glyph) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

// =============================================================================
//  2. Parse tree and parser
// =============================================================================
struct Node;
using NodeP = std::shared_ptr<Node>;

struct Node {
    enum Kind { Atom, Group, Frac, Sqrt, Script, LeftRight, Matrix, Accent, Line, Brace,
                Text, Space, StyleSw, Color, Stack, XArrow, Phantom, SizedDelim, Boxed } kind = Group;
    QVector<uint>  cps;                // Atom/Text code points
    int            cls = Ord;
    bool           bigOp = false;
    int            limits = 0;         // 0 never, 1 always, 2 only in display style
    QVector<NodeP> kids;
    QStringList    delims;             // LeftRight delimiters (code point text, "." = none)
    QVector<QVector<NodeP>> grid;      // Matrix rows -> cells
    QString        env;                // Matrix environment / accent kind
    QString        colSpec;            // Matrix column alignment letters
    qreal          num = 0;            // Space width, delimiter size factor
    bool           flag = false;       // Frac: no rule; Line/Brace: under; Accent: wide; Phantom: horizontal only...
    int            level = -1;         // forced style level (Frac, StyleSw)
    QColor         color;
    bool           error = false;
    int            phantomKind = 0;    // 0 full, 1 h-only, 2 v-only
};

NodeP mk(Node::Kind k) { auto n = std::make_shared<Node>(); n->kind = k; return n; }

// ---- Math alphabets (Unicode "Mathematical Alphanumeric Symbols") --------------
enum FontStyle { FS_Italic, FS_Upright, FS_Bold, FS_BoldItalic, FS_Bb, FS_Cal, FS_Frak, FS_Sans, FS_Mono };

uint styledLatin(uint c, FontStyle fs)
{
    const bool up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z';
    if (!up && !lo) {
        if (c >= '0' && c <= '9') {
            const uint d = c - '0';
            switch (fs) { case FS_Bold: case FS_BoldItalic: return 0x1D7CE + d; case FS_Bb: return 0x1D7D8 + d;
                          case FS_Sans: return 0x1D7E2 + d; case FS_Mono: return 0x1D7F6 + d; default: return c; }
        }
        return c;
    }
    const uint i = up ? c - 'A' : c - 'a';
    switch (fs) {
    case FS_Upright: return c;
    case FS_Italic:  if (c == 'h') return 0x210E; return (up ? 0x1D434 : 0x1D44E) + i;
    case FS_Bold:    return (up ? 0x1D400 : 0x1D41A) + i;
    case FS_BoldItalic: return (up ? 0x1D468 : 0x1D482) + i;
    case FS_Sans:    return (up ? 0x1D5A0 : 0x1D5BA) + i;
    case FS_Mono:    return (up ? 0x1D670 : 0x1D68A) + i;
    case FS_Bb: {
        static const QHash<uint, uint> ex = {{'C',0x2102},{'H',0x210D},{'N',0x2115},{'P',0x2119},{'Q',0x211A},{'R',0x211D},{'Z',0x2124}};
        if (ex.contains(c)) return ex[c];
        return (up ? 0x1D538 : 0x1D552) + i; }
    case FS_Cal: {
        static const QHash<uint, uint> ex = {{'B',0x212C},{'E',0x2130},{'F',0x2131},{'H',0x210B},{'I',0x2110},{'L',0x2112},{'M',0x2133},{'R',0x211B},
                                             {'e',0x212F},{'g',0x210A},{'o',0x2134}};
        if (ex.contains(c)) return ex[c];
        return (up ? 0x1D49C : 0x1D4B6) + i; }
    case FS_Frak: {
        static const QHash<uint, uint> ex = {{'C',0x212D},{'H',0x210C},{'I',0x2111},{'R',0x211C},{'Z',0x2128}};
        if (ex.contains(c)) return ex[c];
        return (up ? 0x1D504 : 0x1D51E) + i; }
    }
    return c;
}

bool isGreekLower(uint c)
{
    return (c >= 0x3B1 && c <= 0x3C9) || c == 0x3D1 || c == 0x3D5 || c == 0x3D6 || c == 0x3F0 || c == 0x3F1 || c == 0x3F5;
}
uint styledGreek(uint c, FontStyle fs)
{
    const bool lower = isGreekLower(c), upper = c >= 0x391 && c <= 0x3A9;
    if (!lower && !upper) return c;
    if (lower) {
        // index inside the Unicode math-Greek alphabets: alpha..omega = 0..24, then the "variant" letters
        uint idx = c - 0x3B1;
        switch (c) { case 0x3F5: idx = 26; break; case 0x3D1: idx = 27; break; case 0x3F0: idx = 28; break;
                     case 0x3D5: idx = 29; break; case 0x3F1: idx = 30; break; case 0x3D6: idx = 31; break; default: break; }
        switch (fs) { case FS_Italic: return 0x1D6FC + idx; case FS_Bold: return 0x1D6C2 + idx;
                      case FS_BoldItalic: return 0x1D736 + idx; default: return c; }
    }
    // capital Greek letters stay upright in TeX unless bold
    if (fs == FS_Bold)       return 0x1D6A8 + (c - 0x391);
    if (fs == FS_BoldItalic) return 0x1D71C + (c - 0x391);
    return c;
}

// ---- Symbol tables --------------------------------------------------------------
struct SymDef { const char *name; uint cp; int cls; };
const SymDef kSyms[] = {
    // Greek
    {"alpha",0x3B1,Ord},{"beta",0x3B2,Ord},{"gamma",0x3B3,Ord},{"delta",0x3B4,Ord},{"epsilon",0x3F5,Ord},{"varepsilon",0x3B5,Ord},
    {"zeta",0x3B6,Ord},{"eta",0x3B7,Ord},{"theta",0x3B8,Ord},{"vartheta",0x3D1,Ord},{"iota",0x3B9,Ord},{"kappa",0x3BA,Ord},
    {"varkappa",0x3F0,Ord},{"lambda",0x3BB,Ord},{"mu",0x3BC,Ord},{"nu",0x3BD,Ord},{"xi",0x3BE,Ord},{"omicron",0x3BF,Ord},
    {"pi",0x3C0,Ord},{"varpi",0x3D6,Ord},{"rho",0x3C1,Ord},{"varrho",0x3F1,Ord},{"sigma",0x3C3,Ord},{"varsigma",0x3C2,Ord},
    {"tau",0x3C4,Ord},{"upsilon",0x3C5,Ord},{"phi",0x3D5,Ord},{"varphi",0x3C6,Ord},{"chi",0x3C7,Ord},{"psi",0x3C8,Ord},{"omega",0x3C9,Ord},
    {"Gamma",0x393,Ord},{"Delta",0x394,Ord},{"Theta",0x398,Ord},{"Lambda",0x39B,Ord},{"Xi",0x39E,Ord},{"Pi",0x3A0,Ord},
    {"Sigma",0x3A3,Ord},{"Upsilon",0x3A5,Ord},{"Phi",0x3A6,Ord},{"Psi",0x3A8,Ord},{"Omega",0x3A9,Ord},
    // ordinary symbols
    {"infty",0x221E,Ord},{"partial",0x2202,Ord},{"nabla",0x2207,Ord},{"forall",0x2200,Ord},{"exists",0x2203,Ord},{"nexists",0x2204,Ord},
    {"neg",0xAC,Ord},{"lnot",0xAC,Ord},{"emptyset",0x2205,Ord},{"varnothing",0x2205,Ord},{"angle",0x2220,Ord},{"measuredangle",0x2221,Ord},
    {"hbar",0x210F,Ord},{"ell",0x2113,Ord},{"Re",0x211C,Ord},{"Im",0x2111,Ord},{"aleph",0x2135,Ord},{"beth",0x2136,Ord},
    {"imath",0x131,Ord},{"jmath",0x237,Ord},{"prime",0x2032,Ord},{"top",0x22A4,Ord},{"bot",0x22A5,Ord},{"triangle",0x25B3,Ord},
    {"square",0x25A1,Ord},{"blacksquare",0x25A0,Ord},{"Box",0x25A1,Ord},{"flat",0x266D,Ord},{"natural",0x266E,Ord},{"sharp",0x266F,Ord},
    {"wp",0x2118,Ord},{"mho",0x2127,Ord},{"dagger",0x2020,Ord},{"ddagger",0x2021,Ord},{"S",0xA7,Ord},{"P",0xB6,Ord},
    {"checkmark",0x2713,Ord},{"degree",0xB0,Ord},{"clubsuit",0x2663,Ord},{"diamondsuit",0x2662,Ord},{"heartsuit",0x2661,Ord},{"spadesuit",0x2660,Ord},
    {"surd",0x221A,Ord},{"backslash",0x5C,Ord},{"lbrace",0x7B,Open},{"rbrace",0x7D,Close},{"langle",0x27E8,Open},{"rangle",0x27E9,Close},
    {"lfloor",0x230A,Open},{"rfloor",0x230B,Close},{"lceil",0x2308,Open},{"rceil",0x2309,Close},{"lvert",0x7C,Open},{"rvert",0x7C,Close},
    {"vert",0x7C,Ord},{"Vert",0x2016,Ord},{"lVert",0x2016,Open},{"rVert",0x2016,Close},
    {"ldots",0x2026,Inner},{"dots",0x2026,Inner},{"cdots",0x22EF,Inner},{"dotsb",0x22EF,Inner},{"vdots",0x22EE,Ord},{"ddots",0x22F1,Ord},
    {"cdotp",0x22C5,Ord},{"colon",0x3A,Punct},
    // binary operators
    {"pm",0xB1,Bin},{"mp",0x2213,Bin},{"times",0xD7,Bin},{"div",0xF7,Bin},{"cdot",0x22C5,Bin},{"ast",0x2217,Bin},{"star",0x22C6,Bin},
    {"circ",0x2218,Bin},{"bullet",0x2219,Bin},{"oplus",0x2295,Bin},{"ominus",0x2296,Bin},{"otimes",0x2297,Bin},{"oslash",0x2298,Bin},
    {"odot",0x2299,Bin},{"cap",0x2229,Bin},{"cup",0x222A,Bin},{"vee",0x2228,Bin},{"lor",0x2228,Bin},{"wedge",0x2227,Bin},{"land",0x2227,Bin},
    {"setminus",0x2216,Bin},{"smallsetminus",0x2216,Bin},{"uplus",0x228E,Bin},{"sqcap",0x2293,Bin},{"sqcup",0x2294,Bin},{"wr",0x2240,Bin},
    {"diamond",0x22C4,Bin},{"triangleleft",0x25C1,Bin},{"triangleright",0x25B7,Bin},{"amalg",0x2A3F,Bin},{"bigtriangleup",0x25B3,Bin},
    {"bigtriangledown",0x25BD,Bin},{"boxplus",0x229E,Bin},{"boxtimes",0x22A0,Bin},{"dotplus",0x2214,Bin},{"ltimes",0x22C9,Bin},{"rtimes",0x22CA,Bin},
    // relations
    {"leq",0x2264,Rel},{"le",0x2264,Rel},{"geq",0x2265,Rel},{"ge",0x2265,Rel},{"neq",0x2260,Rel},{"ne",0x2260,Rel},{"approx",0x2248,Rel},
    {"equiv",0x2261,Rel},{"sim",0x223C,Rel},{"simeq",0x2243,Rel},{"cong",0x2245,Rel},{"propto",0x221D,Rel},{"in",0x2208,Rel},{"notin",0x2209,Rel},
    {"ni",0x220B,Rel},{"subset",0x2282,Rel},{"supset",0x2283,Rel},{"subseteq",0x2286,Rel},{"supseteq",0x2287,Rel},{"subsetneq",0x228A,Rel},
    {"supsetneq",0x228B,Rel},{"nsubseteq",0x2288,Rel},{"nsupseteq",0x2289,Rel},{"ll",0x226A,Rel},{"gg",0x226B,Rel},{"perp",0x22A5,Rel},
    {"parallel",0x2225,Rel},{"nparallel",0x2226,Rel},{"mid",0x2223,Rel},{"nmid",0x2224,Rel},{"vdash",0x22A2,Rel},{"dashv",0x22A3,Rel},
    {"models",0x22A8,Rel},{"asymp",0x224D,Rel},{"doteq",0x2250,Rel},{"prec",0x227A,Rel},{"succ",0x227B,Rel},{"preceq",0x2AAF,Rel},
    {"succeq",0x2AB0,Rel},{"sqsubset",0x228F,Rel},{"sqsubseteq",0x2291,Rel},{"sqsupset",0x2290,Rel},{"sqsupseteq",0x2292,Rel},
    {"lesssim",0x2272,Rel},{"gtrsim",0x2273,Rel},{"leqslant",0x2A7D,Rel},{"geqslant",0x2A7E,Rel},{"ncong",0x2247,Rel},{"nleq",0x2270,Rel},
    {"ngeq",0x2271,Rel},{"nless",0x226E,Rel},{"ngtr",0x226F,Rel},{"nsim",0x2241,Rel},{"lneq",0x2A87,Rel},{"gneq",0x2A88,Rel},
    {"triangleq",0x225C,Rel},{"bowtie",0x22C8,Rel},{"smile",0x2323,Rel},{"frown",0x2322,Rel},
    // arrows
    {"to",0x2192,Rel},{"rightarrow",0x2192,Rel},{"leftarrow",0x2190,Rel},{"gets",0x2190,Rel},{"leftrightarrow",0x2194,Rel},
    {"Rightarrow",0x21D2,Rel},{"Leftarrow",0x21D0,Rel},{"Leftrightarrow",0x21D4,Rel},{"implies",0x27F9,Rel},{"impliedby",0x27F8,Rel},
    {"iff",0x27FA,Rel},{"mapsto",0x21A6,Rel},{"longmapsto",0x27FC,Rel},{"longrightarrow",0x27F6,Rel},{"longleftarrow",0x27F5,Rel},
    {"Longrightarrow",0x27F9,Rel},{"Longleftarrow",0x27F8,Rel},{"longleftrightarrow",0x27F7,Rel},{"Longleftrightarrow",0x27FA,Rel},
    {"uparrow",0x2191,Rel},{"downarrow",0x2193,Rel},{"updownarrow",0x2195,Rel},{"Uparrow",0x21D1,Rel},{"Downarrow",0x21D3,Rel},
    {"Updownarrow",0x21D5,Rel},{"nearrow",0x2197,Rel},{"searrow",0x2198,Rel},{"swarrow",0x2199,Rel},{"nwarrow",0x2196,Rel},
    {"hookrightarrow",0x21AA,Rel},{"hookleftarrow",0x21A9,Rel},{"rightharpoonup",0x21C0,Rel},{"leftharpoonup",0x21BC,Rel},
    {"rightharpoondown",0x21C1,Rel},{"leftharpoondown",0x21BD,Rel},{"rightleftharpoons",0x21CC,Rel},{"leadsto",0x21DD,Rel},
    {"rightsquigarrow",0x21DD,Rel},{"twoheadrightarrow",0x21A0,Rel},{"nrightarrow",0x219B,Rel},{"nleftarrow",0x219A,Rel},
    {"circlearrowleft",0x21BA,Rel},{"circlearrowright",0x21BB,Rel},{"curvearrowright",0x21B7,Rel},{"curvearrowleft",0x21B6,Rel},
};

struct OpDef { const char *name; uint cp; int limits; };      // large operators (limits: 0 never, 2 display only)
const OpDef kBigOps[] = {
    {"sum",0x2211,2},{"prod",0x220F,2},{"coprod",0x2210,2},{"int",0x222B,0},{"iint",0x222C,0},{"iiint",0x222D,0},{"oint",0x222E,0},
    {"oiint",0x222F,0},{"bigcup",0x22C3,2},{"bigcap",0x22C2,2},{"bigvee",0x22C1,2},{"bigwedge",0x22C0,2},{"bigoplus",0x2A01,2},
    {"bigotimes",0x2A02,2},{"bigodot",0x2A00,2},{"biguplus",0x2A04,2},{"bigsqcup",0x2A06,2},
};
const char *kFuncs[] = {"sin","cos","tan","cot","sec","csc","arcsin","arccos","arctan","sinh","cosh","tanh","coth","log","ln","lg","exp",
                        "ker","deg","hom","arg","dim","lim","limsup","liminf","max","min","sup","inf","det","gcd","Pr","argmax","argmin"};
const char *kLimFuncs[] = {"lim","limsup","liminf","max","min","sup","inf","det","gcd","Pr","argmax","argmin"};

struct AccentDef { const char *name; uint cp; bool wide; };
const AccentDef kAccents[] = {
    {"hat",0x302,false},{"widehat",0x302,true},{"tilde",0x303,false},{"widetilde",0x303,true},{"bar",0x304,false},{"vec",0x20D7,false},
    {"dot",0x307,false},{"ddot",0x308,false},{"dddot",0x20DB,false},{"check",0x30C,false},{"breve",0x306,false},{"acute",0x301,false},
    {"grave",0x300,false},{"mathring",0x30A,false},{"overrightarrow",0x20D7,true},
};

// \not mapping to a pre-composed negated symbol
const QHash<uint, uint> &negations()
{
    static const QHash<uint, uint> m = {
        {'=',0x2260},{'<',0x226E},{'>',0x226F},{0x2264,0x2270},{0x2265,0x2271},{0x2208,0x2209},{0x2282,0x2284},{0x2283,0x2285},
        {0x2286,0x2288},{0x2287,0x2289},{0x2261,0x2262},{0x223C,0x2241},{0x2248,0x2249},{0x2245,0x2247},{0x2223,0x2224},{0x2225,0x2226},
        {0x2192,0x219B},{0x21D2,0x21CF},{0x2194,0x21AE},{0x21D4,0x21CE},{0x2203,0x2204},{0x22A2,0x22AC},{0x22A8,0x22AD},{0x220B,0x220C}};
    return m;
}

QColor namedColor(const QString &n)
{
    static const QHash<QString, QString> m = {{"red","#d32f2f"},{"blue","#1565c0"},{"green","#2e7d32"},{"orange","#ef6c00"},{"purple","#7b1fa2"},
        {"gray","#757575"},{"grey","#757575"},{"black","#000000"},{"white","#ffffff"},{"cyan","#00acc1"},{"magenta","#c2185b"},{"yellow","#f9a825"},
        {"brown","#6d4c41"},{"teal","#00897b"},{"pink","#d81b60"}};
    if (m.contains(n.toLower())) return QColor(m[n.toLower()]);
    QColor c(n.startsWith('#') ? n : "#" + n);
    return c.isValid() ? c : QColor();
}

// ---- Parser --------------------------------------------------------------------
class Parser {
public:
    explicit Parser(const QString &s, FontStyle fs = FS_Italic) : m_src(s), m_fs(fs) {}

    NodeP parseAll()
    {
        auto grid = parseGrid(QString());
        if (grid.size() == 1 && grid[0].size() == 1) return grid[0][0];
        auto m = mk(Node::Matrix);
        bool hasAmp = false;
        for (const auto &row : grid) if (row.size() > 1) hasAmp = true;
        m->env = hasAmp ? "align" : "gather";
        m->grid = grid;
        return m;
    }

private:
    QString m_src;
    int m_pos = 0;
    FontStyle m_fs;

    // stop reasons
    enum Stop { StopEof, StopBrace, StopAmp, StopRowEnd, StopEnd, StopRight, StopMiddle };
    Stop m_stop = StopEof;

    bool eof() const { return m_pos >= m_src.size(); }
    void skipSpaces() { while (!eof() && m_src[m_pos].isSpace()) ++m_pos; }

    QString readCommand()          // assumes m_src[m_pos] == '\\'
    {
        ++m_pos;
        if (eof()) return QString();
        if (m_src[m_pos].isLetter()) {
            int s = m_pos;
            while (!eof() && m_src[m_pos].isLetter()) ++m_pos;
            QString name = m_src.mid(s, m_pos - s);
            if (!eof() && m_src[m_pos] == '*' && (name == "operatorname")) { ++m_pos; name += "*"; }
            return name;
        }
        return QString(m_src[m_pos++]);
    }

    // ---- sequences -----------------------------------------------------------------
    QVector<NodeP> parseSequence(bool inBrace, bool inCell, bool inDelim)
    {
        QVector<NodeP> out;
        while (true) {
            skipSpaces();
            if (eof()) { m_stop = StopEof; return out; }
            const QChar c = m_src[m_pos];

            if (c == '}') { if (inBrace) { m_stop = StopBrace; return out; } ++m_pos; continue; }
            if (c == '&') { if (inCell) { m_stop = StopAmp; return out; } ++m_pos; continue; }
            if (c == '%') { while (!eof() && m_src[m_pos] != '\n') ++m_pos; continue; }   // comment

            NodeP node;
            if (c == '\\') {
                const int save = m_pos;
                const QString cmd = readCommand();
                if (cmd == "\\") { if (inCell) { skipRowOptions(); m_stop = StopRowEnd; return out; } continue; }
                if (cmd == "end")    { m_pos = save; m_stop = StopEnd;    return out; }
                if (cmd == "right")  { m_pos = save; m_stop = StopRight;  return out; }
                if (cmd == "middle") { m_pos = save; m_stop = StopMiddle; return out; }
                if (cmd == "limits" || cmd == "nolimits") {
                    if (!out.isEmpty()) setLimits(out.last(), cmd == "limits" ? 1 : 0);
                    continue;
                }
                node = parseCommand(cmd);
                if (!node) continue;
            } else if (c == '{') {
                ++m_pos;
                node = group(parseSequence(true, false, false));
                if (!eof() && m_src[m_pos] == '}') ++m_pos;
            } else if (c == '^' || c == '_' || c == QChar('\'')) {
                node = mk(Node::Group);       // empty base: "^2" or a leading prime
            } else if (c == '$') {
                ++m_pos; continue;
            } else {
                node = charAtom(c);
                ++m_pos;
                if (c == '~') { node = mk(Node::Space); node->num = 333; }
                if (!node) continue;
            }
            node = attachScripts(node);
            out.append(node);
        }
    }

    NodeP group(const QVector<NodeP> &kids)
    {
        auto g = mk(Node::Group);
        g->kids = kids;
        return g;
    }
    static void setLimits(const NodeP &n, int v)
    {
        if (n->kind == Node::Script && !n->kids.isEmpty()) { setLimits(n->kids[0], v); return; }
        n->limits = v;
    }
    void skipRowOptions()                 // "\\[3pt]"
    {
        skipSpaces();
        if (!eof() && m_src[m_pos] == '[') { while (!eof() && m_src[m_pos] != ']') ++m_pos; if (!eof()) ++m_pos; }
    }

    // ---- scripts ---------------------------------------------------------------------
    NodeP attachScripts(NodeP base)
    {
        NodeP sub, sup;
        while (true) {
            skipSpaces();
            if (eof()) break;
            const QChar c = m_src[m_pos];
            if (c == QChar('\'')) {                              // f' f'' f'''
                int n = 0;
                while (!eof() && m_src[m_pos] == QChar('\'')) { ++n; ++m_pos; }
                auto p = mk(Node::Atom);
                if (n == 1) p->cps = {0x2032}; else if (n == 2) p->cps = {0x2033}; else if (n == 3) p->cps = {0x2034};
                else for (int i = 0; i < n; ++i) p->cps << 0x2032;
                sup = sup ? group({sup, p}) : p;
            } else if (c == '^') {
                ++m_pos;
                NodeP arg = scriptArg();
                sup = sup ? group({sup, arg}) : arg;          // f'^2  =>  superscript "prime 2"
            } else if (c == '_') {
                ++m_pos;
                sub = scriptArg();
            } else break;
        }
        if (!sub && !sup) return base;
        auto s = mk(Node::Script);
        s->kids = {base, sub, sup};
        return s;
    }
    NodeP scriptArg()
    {
        skipSpaces();
        if (eof()) return mk(Node::Group);
        const QChar c = m_src[m_pos];
        if (c == '{') {
            ++m_pos;
            auto g = group(parseSequence(true, false, false));
            if (!eof() && m_src[m_pos] == '}') ++m_pos;
            return g;
        }
        if (c == '\\') {
            const QString cmd = readCommand();
            auto n = parseCommand(cmd);
            return n ? n : mk(Node::Group);
        }
        ++m_pos;
        return charAtom(c);
    }
    // one braced group or a single token
    NodeP argument()
    {
        skipSpaces();
        if (eof()) return mk(Node::Group);
        if (m_src[m_pos] == '{') {
            ++m_pos;
            auto g = group(parseSequence(true, false, false));
            if (!eof() && m_src[m_pos] == '}') ++m_pos;
            return g;
        }
        return scriptArg();
    }
    QString rawGroup()                    // text between braces, nesting aware
    {
        skipSpaces();
        if (eof() || m_src[m_pos] != '{') { if (eof()) return QString(); return QString(m_src[m_pos++]); }
        int depth = 0; QString out;
        while (!eof()) {
            const QChar c = m_src[m_pos++];
            if (c == '{') { if (depth++ > 0) out += c; }
            else if (c == '}') { if (--depth == 0) break; out += c; }
            else out += c;
        }
        return out;
    }
    QString optionalBracket()
    {
        skipSpaces();
        if (eof() || m_src[m_pos] != '[') return QString();
        ++m_pos;
        QString out; int depth = 0;
        while (!eof()) {
            const QChar c = m_src[m_pos++];
            if (c == '{') ++depth; else if (c == '}') --depth;
            if (c == ']' && depth == 0) break;
            out += c;
        }
        return out;
    }
    NodeP parseSub(const QString &s, FontStyle fs)
    {
        Parser p(s, fs);
        return p.parseAll();
    }

    // ---- atoms -------------------------------------------------------------------------
    NodeP atomOf(uint cp, int cls)
    {
        auto a = mk(Node::Atom);
        a->cps = {cp};
        a->cls = cls;
        return a;
    }
    NodeP charAtom(QChar qc)
    {
        const uint c = qc.unicode();
        if (qc.isLetter() || qc.isDigit()) {
            if (c < 128) return atomOf(styledLatin(c, m_fs), Ord);
            if (isGreekLower(c) || (c >= 0x391 && c <= 0x3A9)) return atomOf(styledGreek(c, m_fs), Ord);
            return atomOf(c, Ord);
        }
        switch (c) {
        case '+': return atomOf('+', Bin);
        case '-': return atomOf(0x2212, Bin);
        case '*': return atomOf(0x2217, Bin);
        case '=': return atomOf('=', Rel);
        case '<': case '>': return atomOf(c, Rel);
        case ':': return atomOf(':', Rel);
        case ',': case ';': return atomOf(c, Punct);
        case '(': case '[': return atomOf(c, Open);
        case ')': case ']': return atomOf(c, Close);
        case '|': return atomOf('|', Ord);
        case '\'': return atomOf(0x2032, Ord);
        case '~': return nullptr;
        default: return atomOf(c, Ord);
        }
    }

    NodeP textNode(const QString &s, FontStyle fs)
    {
        auto t = mk(Node::Text);
        for (const QChar &q : s) {
            const uint c = q.unicode();
            t->cps << ((c < 128) ? (fs == FS_Upright ? c : styledLatin(c, fs)) : c);
        }
        return t;
    }

    NodeP withFont(FontStyle fs)
    {
        const FontStyle old = m_fs;
        m_fs = fs;
        NodeP n = argument();
        m_fs = old;
        return n;
    }

    // ---- commands ----------------------------------------------------------------------
    NodeP parseCommand(const QString &cmd)
    {
        if (cmd.isEmpty()) return nullptr;

        // --- symbols
        static const QHash<QString, const SymDef *> syms = [] {
            QHash<QString, const SymDef *> h;
            for (const SymDef &s : kSyms) h.insert(QString::fromLatin1(s.name), &s);
            return h;
        }();
        if (auto it = syms.find(cmd); it != syms.end()) {
            const SymDef *s = it.value();
            uint cp = s->cp;
            if (isGreekLower(cp)) cp = styledGreek(cp, m_fs);
            else if (cp >= 0x391 && cp <= 0x3A9) cp = styledGreek(cp, m_fs);
            return atomOf(cp, s->cls);
        }
        for (const OpDef &o : kBigOps)
            if (cmd == o.name) { auto a = atomOf(o.cp, Op); a->bigOp = true; a->limits = o.limits; return a; }
        for (const char *f : kFuncs)
            if (cmd == f) {
                auto t = textNode(cmd, FS_Upright);
                t->kind = Node::Atom; t->cls = Op;
                for (const char *l : kLimFuncs) if (cmd == l) t->limits = 2;
                return t;
            }
        for (const AccentDef &a : kAccents)
            if (cmd == a.name) { auto n = mk(Node::Accent); n->cps = {a.cp}; n->flag = a.wide; n->kids = {argument()}; return n; }

        // --- fractions, roots
        if (cmd == "frac" || cmd == "dfrac" || cmd == "tfrac" || cmd == "cfrac") {
            auto n = mk(Node::Frac);
            n->level = cmd == "dfrac" || cmd == "cfrac" ? 0 : (cmd == "tfrac" ? 1 : -1);
            n->kids = {argument(), argument()};
            return n;
        }
        if (cmd == "binom" || cmd == "dbinom" || cmd == "tbinom") {
            auto f = mk(Node::Frac); f->flag = true;
            f->level = cmd == "dbinom" ? 0 : (cmd == "tbinom" ? 1 : -1);
            f->kids = {argument(), argument()};
            auto lr = mk(Node::LeftRight);
            lr->kids = {group({f})};
            lr->delims = {QString::number(uint('(')), QString::number(uint(')'))};
            return lr;
        }
        if (cmd == "sqrt") {
            auto n = mk(Node::Sqrt);
            const QString idx = optionalBracket();
            n->kids = {argument()};
            if (!idx.isEmpty()) n->kids << parseSub(idx, m_fs);
            return n;
        }
        // --- delimiters
        if (cmd == "left") return parseLeftRight();
        if (cmd == "big" || cmd == "Big" || cmd == "bigg" || cmd == "Bigg" || cmd == "bigl" || cmd == "Bigl" || cmd == "biggl" || cmd == "Biggl" ||
            cmd == "bigr" || cmd == "Bigr" || cmd == "biggr" || cmd == "Biggr" || cmd == "bigm" || cmd == "Bigm" || cmd == "biggm" || cmd == "Biggm") {
            auto n = mk(Node::SizedDelim);
            const QString base = cmd.left(cmd.size() - ((cmd.endsWith('l') || cmd.endsWith('r') || cmd.endsWith('m')) && cmd != "bigg" ? 1 : 0));
            n->num = base == "big" ? 1200 : base == "Big" ? 1800 : base == "bigg" ? 2400 : 3000;
            n->cls = cmd.endsWith('l') ? Open : cmd.endsWith('r') ? Close : Ord;
            n->cps = {delimCp(readDelimiter())};
            return n;
        }
        if (cmd == "{" || cmd == "}") return atomOf(cmd[0].unicode(), cmd == "{" ? Open : Close);
        if (cmd == "|") return atomOf(0x2016, Ord);
        if (cmd == "$" || cmd == "%" || cmd == "#" || cmd == "&" || cmd == "_") return atomOf(cmd[0].unicode(), Ord);
        if (cmd == "/") return nullptr;
        // --- spacing
        if (cmd == ",") return space(167);
        if (cmd == ":" || cmd == ">") return space(222);
        if (cmd == ";") return space(278);
        if (cmd == "!") return space(-167);
        if (cmd == " ") return space(333);
        if (cmd == "quad") return space(1000);
        if (cmd == "qquad") return space(2000);
        if (cmd == "enspace") return space(500);
        if (cmd == "thinspace") return space(167);
        if (cmd == "medspace") return space(222);
        if (cmd == "thickspace") return space(278);
        if (cmd == "negthinspace") return space(-167);
        if (cmd == "hspace" || cmd == "mspace" || cmd == "kern" || cmd == "hskip") return space(parseDimension(rawGroup()));
        if (cmd == "mathstrut" || cmd == "strut") { auto p = mk(Node::Phantom); p->phantomKind = 2; p->kids = {textNode("(", FS_Upright)}; return p; }
        // --- fonts / text
        if (cmd == "text" || cmd == "textrm" || cmd == "mbox" || cmd == "hbox") return textNode(rawGroup(), FS_Upright);
        if (cmd == "textbf") return textNode(rawGroup(), FS_Bold);
        if (cmd == "textit" || cmd == "emph") return textNode(rawGroup(), FS_Italic);
        if (cmd == "textsf") return textNode(rawGroup(), FS_Sans);
        if (cmd == "texttt") return textNode(rawGroup(), FS_Mono);
        if (cmd == "mathrm" || cmd == "rm") return withFont(FS_Upright);
        if (cmd == "mathbf" || cmd == "bf" || cmd == "mathbfup") return withFont(FS_Bold);
        if (cmd == "boldsymbol" || cmd == "bm" || cmd == "pmb" || cmd == "mathbfit") return withFont(FS_BoldItalic);
        if (cmd == "mathit" || cmd == "it") return withFont(FS_Italic);
        if (cmd == "mathsf") return withFont(FS_Sans);
        if (cmd == "mathtt") return withFont(FS_Mono);
        if (cmd == "mathbb" || cmd == "Bbb") return withFont(FS_Bb);
        if (cmd == "mathcal" || cmd == "mathscr" || cmd == "cal") return withFont(FS_Cal);
        if (cmd == "mathfrak") return withFont(FS_Frak);
        if (cmd == "operatorname" || cmd == "operatorname*") {
            auto t = textNode(rawGroup(), FS_Upright);
            t->kind = Node::Atom; t->cls = Op;
            if (cmd.endsWith('*')) t->limits = 2;
            return t;
        }
        if (cmd == "mathbin" || cmd == "mathrel" || cmd == "mathop" || cmd == "mathord" || cmd == "mathpunct" || cmd == "mathopen" || cmd == "mathclose" || cmd == "mathinner") {
            auto g = argument();
            auto w = mk(Node::Group); w->kids = {g};
            w->cls = cmd == "mathbin" ? Bin : cmd == "mathrel" ? Rel : cmd == "mathop" ? Op : cmd == "mathpunct" ? Punct :
                     cmd == "mathopen" ? Open : cmd == "mathclose" ? Close : cmd == "mathinner" ? Inner : Ord;
            if (cmd == "mathop") w->limits = 2;
            return w;
        }
        if (cmd == "bmod")  { auto t = textNode("mod", FS_Upright); t->kind = Node::Atom; t->cls = Bin; return t; }
        if (cmd == "mod")   { auto g = mk(Node::Group); g->kids = {space(1000), textNode("mod", FS_Upright), space(333), argument()}; return g; }
        if (cmd == "pmod")  {
            auto g = mk(Node::Group);
            g->kids = {space(1000), atomOf('(', Open), textNode("mod", FS_Upright), space(333), argument(), atomOf(')', Close)};
            return g;
        }
        if (cmd == "ensuremath") return argument();
        // --- lines, braces, stacking
        if (cmd == "overline" || cmd == "underline") { auto n = mk(Node::Line); n->flag = cmd == "underline"; n->kids = {argument()}; return n; }
        if (cmd == "overbrace" || cmd == "underbrace") { auto n = mk(Node::Brace); n->flag = cmd == "underbrace"; n->kids = {argument()}; n->limits = 1; return n; }
        if (cmd == "overset" || cmd == "stackrel") { auto n = mk(Node::Stack); auto top = argument(); auto base = argument(); n->kids = {base, top}; n->limits = 1; return n; }
        if (cmd == "underset") { auto n = mk(Node::Stack); auto bot = argument(); auto base = argument(); n->kids = {base, nullptr, bot}; n->flag = true; n->limits = 1; return n; }
        if (cmd == "xrightarrow" || cmd == "xleftarrow" || cmd == "xRightarrow" || cmd == "xLeftarrow" || cmd == "xleftrightarrow") {
            auto n = mk(Node::XArrow);
            n->env = cmd;
            const QString below = optionalBracket();
            n->kids = {argument(), below.isEmpty() ? NodeP() : parseSub(below, m_fs)};
            return n;
        }
        if (cmd == "substack") {
            skipSpaces();
            auto m = mk(Node::Matrix); m->env = "substack"; m->colSpec = "c";
            QString body = rawGroup();
            Parser p(body, m_fs);
            m->grid = p.parseGrid(QString());
            return m;
        }
        if (cmd == "boxed") { auto n = mk(Node::Boxed); n->kids = {argument()}; return n; }
        if (cmd == "phantom" || cmd == "hphantom" || cmd == "vphantom") {
            auto n = mk(Node::Phantom); n->phantomKind = cmd == "phantom" ? 0 : cmd == "hphantom" ? 1 : 2; n->kids = {argument()}; return n;
        }
        if (cmd == "not") {
            skipSpaces();
            NodeP next = eof() ? nullptr : (m_src[m_pos] == '\\' ? parseCommand(readCommand()) : (++m_pos, charAtom(m_src[m_pos - 1])));
            if (next && next->kind == Node::Atom && next->cps.size() == 1 && negations().contains(next->cps[0])) next->cps = {negations()[next->cps[0]]};
            return next;
        }
        // --- styles & colour
        if (cmd == "displaystyle" || cmd == "textstyle" || cmd == "scriptstyle" || cmd == "scriptscriptstyle") {
            auto n = mk(Node::StyleSw);
            n->level = cmd == "displaystyle" ? 0 : cmd == "textstyle" ? 1 : cmd == "scriptstyle" ? 2 : 3;
            return n;
        }
        if (cmd == "color") { auto n = mk(Node::Color); n->color = namedColor(rawGroup()); return n; }
        if (cmd == "textcolor" || cmd == "colorbox") { auto n = mk(Node::Color); n->color = namedColor(rawGroup()); n->kids = {argument()}; return n; }
        // --- environments
        if (cmd == "begin") return parseEnvironment();
        // --- things we silently ignore
        if (cmd == "label" || cmd == "tag" || cmd == "tag*" || cmd == "ref" || cmd == "eqref") { rawGroup(); return nullptr; }
        if (cmd == "nonumber" || cmd == "notag" || cmd == "hline" || cmd == "hdashline" || cmd == "displaylines" || cmd == "protect" ||
            cmd == "relax" || cmd == "allowbreak" || cmd == "nobreak" || cmd == "vphantom0" || cmd == "bigl" ) return nullptr;
        if (cmd == "cr") return nullptr;

        // unknown command: show it in red so the user sees what went wrong
        auto t = textNode("\\" + cmd, FS_Upright);
        t->error = true;
        return t;
    }

    NodeP space(qreal w) { auto n = mk(Node::Space); n->num = w; return n; }

    static qreal parseDimension(const QString &s)
    {
        static const QRegularExpression re(R"(^\s*(-?[0-9]*\.?[0-9]+)\s*(em|ex|pt|mu|px|cm|mm|in)?)");
        const auto m = re.match(s);
        if (!m.hasMatch()) return 0;
        const qreal v = m.captured(1).toDouble();
        const QString u = m.captured(2);
        if (u == "em" || u.isEmpty()) return v * 1000;
        if (u == "ex") return v * 431;
        if (u == "pt") return v * 100;        // 10pt font: 1pt = 0.1em
        if (u == "mu") return v * 55.5;
        if (u == "px") return v * 75;
        if (u == "cm") return v * 2845 * 0.1; // 1cm ~ 28.45pt
        if (u == "mm") return v * 284.5 * 0.1;
        return v * 7227 * 0.1;                // inches
    }

    // ---- \left ... \right ------------------------------------------------------------
    QString readDelimiter()
    {
        skipSpaces();
        if (eof()) return ".";
        if (m_src[m_pos] == '\\') {
            const QString c = readCommand();
            return c.isEmpty() ? QString(".") : "\\" + c;
        }
        return QString(m_src[m_pos++]);
    }
    static uint delimCp(const QString &d)
    {
        static const QHash<QString, uint> m = {
            {"(",'('},{")",')'},{"[",'['},{"]",']'},{"<",0x27E8},{">",0x27E9},{"|",'|'},{"/",'/'},{"\\{",'{'},{"\\}",'}'},{"\\|",0x2016},
            {"\\lbrace",'{'},{"\\rbrace",'}'},{"\\langle",0x27E8},{"\\rangle",0x27E9},{"\\lfloor",0x230A},{"\\rfloor",0x230B},
            {"\\lceil",0x2308},{"\\rceil",0x2309},{"\\vert",'|'},{"\\Vert",0x2016},{"\\lvert",'|'},{"\\rvert",'|'},{"\\lVert",0x2016},
            {"\\rVert",0x2016},{"\\backslash",'\\'},{"\\uparrow",0x2191},{"\\downarrow",0x2193},{"\\updownarrow",0x2195},{"\\Uparrow",0x21D1},
            {"\\Downarrow",0x21D3},{"\\Updownarrow",0x21D5},{"\\ulcorner",0x231C},{"\\urcorner",0x231D},{"\\llcorner",0x231E},{"\\lrcorner",0x231F}};
        return m.value(d, 0);
    }
    NodeP parseLeftRight()
    {
        auto n = mk(Node::LeftRight);
        n->delims << QString::number(delimCp(readDelimiter()));
        while (true) {
            n->kids << group(parseSequence(false, false, true));
            if (m_stop == StopMiddle) { readCommand(); n->delims << QString::number(delimCp(readDelimiter())); continue; }
            if (m_stop == StopRight)  { readCommand(); n->delims << QString::number(delimCp(readDelimiter())); break; }
            n->delims << "0";            // missing \right
            break;
        }
        return n;
    }

    // ---- environments --------------------------------------------------------------------
public:
    QVector<QVector<NodeP>> parseGrid(const QString &envName)
    {
        QVector<QVector<NodeP>> rows;
        QVector<NodeP> row;
        while (true) {
            auto cell = group(parseSequence(false, true, false));
            row << cell;
            if (m_stop == StopAmp) { ++m_pos; continue; }
            if (m_stop == StopRowEnd) { rows << row; row.clear(); continue; }
            break;                                   // end / eof
        }
        // drop a trailing empty row created by a final "\\"
        const bool emptyRow = row.size() == 1 && row[0]->kids.isEmpty();
        if (!row.isEmpty() && !(emptyRow && !rows.isEmpty())) rows << row;
        Q_UNUSED(envName);
        return rows;
    }
private:
    NodeP parseEnvironment()
    {
        const QString name = rawGroup();
        QString spec;
        if (name == "array" || name == "alignat" || name == "alignat*" || name == "subarray") spec = rawGroup();
        else if (name == "aligned" || name == "gathered" || name == "matrix*") optionalBracket();

        auto m = mk(Node::Matrix);
        m->env = name;
        m->colSpec = spec;
        m->grid = parseGrid(name);
        // consume \end{name}
        skipSpaces();
        if (!eof() && m_src[m_pos] == '\\') {
            const int save = m_pos;
            if (readCommand() == "end") rawGroup(); else m_pos = save;
        }
        auto wrap = [&](const char *l, const char *r) {
            auto lr = mk(Node::LeftRight);
            lr->kids = {group({m})};
            lr->delims = {QString::number(delimCp(l)), QString::number(delimCp(r))};
            return lr;
        };
        if (name == "pmatrix") return wrap("(", ")");
        if (name == "bmatrix") return wrap("[", "]");
        if (name == "Bmatrix") return wrap("\\{", "\\}");
        if (name == "vmatrix") return wrap("|", "|");
        if (name == "Vmatrix") return wrap("\\|", "\\|");
        if (name == "cases" || name == "dcases") {
            auto lr = mk(Node::LeftRight);
            lr->kids = {group({m})};
            lr->delims = {QString::number(delimCp("\\{")), "0"};
            return lr;
        }
        return m;
    }
};

// =============================================================================
//  3. Layout engine
// =============================================================================
using DrawFn = std::function<void(QPainter &, qreal, qreal)>;

struct Box {
    qreal w = 0, asc = 0, desc = 0;
    qreal ic = 0;               // italic correction
    qreal accentX = -1;         // x of the top-accent attachment point (-1 => w/2)
    int   glyphs = 0;
    DrawFn draw;
};

struct Style {
    int   level = 1;            // 0 display, 1 text, 2 script, 3 scriptscript
    bool  cramped = false;
    QColor color = Qt::black;
    qreal k() const { return level <= 1 ? 1.0 : (level == 2 ? 0.7 : 0.5); }
    bool  display() const { return level == 0; }
    bool  script() const { return level >= 2; }
    Style sup()   const { Style s = *this; s.level = level < 2 ? 2 : 3; return s; }
    Style sub()   const { Style s = sup(); s.cramped = true; return s; }
    Style numer() const { Style s = *this; s.level = level == 0 ? 1 : qMin(3, level + 1); return s; }
    Style denom() const { Style s = numer(); s.cramped = true; return s; }
    Style crampedStyle() const { Style s = *this; s.cramped = true; return s; }
};

// Constants from the font's MATH table (font units, per 1000 em).
namespace C {
constexpr qreal Axis = 250, RuleThickness = 40;
constexpr qreal SupShiftUp = 363, SupShiftUpCramped = 289, SupBottomMin = 108, SupBaselineDropMax = 250;
constexpr qreal SubShiftDown = 247, SubTopMax = 344, SubBaselineDropMin = 200, SubSupGapMin = 160, SupBottomMaxWithSub = 344, SpaceAfterScript = 56;
constexpr qreal UpperLimitGapMin = 200, UpperLimitRiseMin = 111, LowerLimitGapMin = 167, LowerLimitDropMin = 600;
constexpr qreal FracNumUp = 394, FracNumUpD = 677, FracDenDown = 345, FracDenDownD = 686, FracNumGap = 40, FracNumGapD = 120;
constexpr qreal StackTopUp = 444, StackTopUpD = 677, StackBottomDown = 345, StackBottomDownD = 686, StackGap = 120, StackGapD = 280;
constexpr qreal RadicalGap = 50, RadicalGapD = 148, RadicalExtra = 40, RadicalKernBefore = 278, RadicalKernAfter = -556, RadicalRaise = 0.6;
constexpr qreal AccentBaseHeight = 450, DisplayOpMinHeight = 1300;
}

Box spaceBox(qreal w) { Box b; b.w = w; return b; }

Box hcat(const QVector<Box> &v)
{
    Box r;
    QVector<qreal> xs;
    qreal x = 0;
    for (const Box &b : v) {
        xs << x; x += b.w;
        r.asc = qMax(r.asc, b.asc); r.desc = qMax(r.desc, b.desc);
        r.glyphs += b.glyphs;
    }
    if (!v.isEmpty()) r.ic = v.last().ic;
    r.w = x;
    r.draw = [v, xs](QPainter &p, qreal px, qreal py) {
        for (int i = 0; i < v.size(); ++i) if (v[i].draw) v[i].draw(p, px + xs[i], py);
    };
    return r;
}

// A run of characters drawn with the math font.
Box glyphBox(const QVector<uint> &cps, const Style &st, qreal extra = 1.0)
{
    MathFont &f = F();
    const qreal k = st.k() * extra;
    struct G { quint32 g; qreal x; };
    QVector<G> gl;
    qreal x = 0, asc = 0, desc = 0;
    quint32 last = 0;
    for (uint cp : cps) {
        if (cp == ' ' || cp == 0xA0) { x += 333; continue; }
        const quint32 g = f.glyph(cp);
        const QRectF r = f.ink(g);
        if (r.width() > 0 && r.height() > 0) { asc = qMax(asc, -r.top()); desc = qMax(desc, r.bottom()); }
        gl.append({g, x});
        x += f.advance(g);
        last = g;
    }
    Box b;
    b.w = x * k; b.asc = asc * k; b.desc = desc * k; b.glyphs = gl.size();
    int v = 0;
    if (last && lookupValue(kItalicCorrection, kItalicCorrectionCount, last, &v)) b.ic = v * k;
    if (gl.size() == 1 && lookupValue(kTopAccent, kTopAccentCount, last, &v)) b.accentX = v * k;
    const QColor col = st.color;
    b.draw = [gl, k, col](QPainter &p, qreal px, qreal py) {
        for (const G &q : gl) {
            const QPainterPath &path = F().path(q.g);
            if (path.isEmpty()) continue;
            p.save();
            p.translate(px + q.x * k, py);
            p.scale(k, k);
            p.fillPath(path, col);
            p.restore();
        }
    };
    return b;
}

// A single glyph by index, optionally centred on the math axis (delimiters, big operators).
Box glyphIndexBox(quint32 g, const Style &st, bool centerOnAxis)
{
    MathFont &f = F();
    const qreal k = st.k();
    const QRectF r = f.ink(g);
    const qreal dy = centerOnAxis ? -C::Axis - (r.top() + r.bottom()) / 2 : 0;
    Box b;
    b.w = f.advance(g) * k;
    b.asc = -(dy + r.top()) * k;
    b.desc = (dy + r.bottom()) * k;
    b.glyphs = 1;
    int v = 0;
    if (lookupValue(kItalicCorrection, kItalicCorrectionCount, g, &v)) b.ic = v * k;
    const QColor col = st.color;
    b.draw = [g, k, dy, col](QPainter &p, qreal px, qreal py) {
        const QPainterPath &path = F().path(g);
        p.save();
        p.translate(px, py + dy * k);
        p.scale(k, k);
        p.fillPath(path, col);
        p.restore();
    };
    return b;
}

// ---- Glyph assemblies (extensible delimiters, radicals, braces) -------------------------------
struct AsmPart { quint32 glyph; qreal pos; };
struct AsmResult { QVector<AsmPart> parts; qreal total = 0; };

AsmResult layoutAssembly(const MGPart *parts, int n, qreal target)
{
    bool hasExt = false;
    for (int i = 0; i < n; ++i) if (parts[i].extender) hasExt = true;

    auto expand = [&](int e) {
        QVector<const MGPart *> v;
        for (int i = 0; i < n; ++i) {
            if (parts[i].extender) { for (int j = 0; j < e; ++j) v << &parts[i]; }
            else v << &parts[i];
        }
        return v;
    };
    int ext = hasExt ? 1 : 0;
    QVector<const MGPart *> seq;
    qreal sum = 0;
    while (true) {
        seq = expand(ext);
        sum = 0;
        for (auto p : seq) sum += p->fullAdvance;
        const qreal maxLen = sum - kMinConnectorOverlap * (seq.size() - 1);
        if (maxLen >= target || !hasExt || ext > 80) break;
        ++ext;
    }
    const int gaps = seq.size() - 1;
    qreal ov = kMinConnectorOverlap;
    if (gaps > 0) {
        ov = (sum - target) / gaps;
        qreal maxOv = 1e9;
        for (int i = 0; i < gaps; ++i) maxOv = qMin<qreal>(maxOv, qMin(seq[i]->endConn, seq[i + 1]->startConn));
        ov = qBound<qreal>(qMin<qreal>(kMinConnectorOverlap, maxOv), ov, maxOv);
    }
    AsmResult r;
    qreal pos = 0;
    for (int i = 0; i < seq.size(); ++i) {
        r.parts.append({seq[i]->glyph, pos});
        pos += seq[i]->fullAdvance - (i < gaps ? ov : 0);
    }
    r.total = pos;
    return r;
}

// Vertically stretchy character of (at least) `target` font units, centred on the axis.
Box stretchyVertical(uint cp, qreal target, const Style &st)
{
    MathFont &f = F();
    const MGConstruction *c = vertConstruction(cp);
    const quint32 base = f.glyph(cp);
    if (!c) return glyphIndexBox(base, st, true);
    for (int i = 0; i < c->varCount; ++i)
        if (kVertVariants[c->varOff + i].size >= target) return glyphIndexBox(kVertVariants[c->varOff + i].glyph, st, true);
    if (c->partCount == 0)
        return glyphIndexBox(c->varCount ? kVertVariants[c->varOff + c->varCount - 1].glyph : base, st, true);

    // assemble from parts (bottom to top), centred on the axis
    const AsmResult a = layoutAssembly(&kVertParts[c->partOff], c->partCount, target);
    const qreal k = st.k();
    const qreal bottomUp = C::Axis - a.total / 2;
    struct P { quint32 g; qreal originUp; };
    QVector<P> ps;
    qreal width = 0;
    for (const AsmPart &p : a.parts) {
        const QRectF r = f.ink(p.glyph);
        ps.append({p.glyph, bottomUp + p.pos + r.bottom()});
        width = qMax(width, f.advance(p.glyph));
    }
    Box b;
    b.w = width * k; b.asc = (C::Axis + a.total / 2) * k; b.desc = (a.total / 2 - C::Axis) * k; b.glyphs = 1;
    const QColor col = st.color;
    b.draw = [ps, k, col](QPainter &p, qreal px, qreal py) {
        for (const P &q : ps) {
            p.save(); p.translate(px, py - q.originUp * k); p.scale(k, k);
            p.fillPath(F().path(q.g), col); p.restore();
        }
    };
    return b;
}

// ---- Layout recursion ---------------------------------------------------------------------------
Box layout(const NodeP &n, const Style &st);
Box layoutSeq(const QVector<NodeP> &seq, Style st);

int classOf(const NodeP &n)
{
    switch (n->kind) {
    case Node::Atom: case Node::Group: case Node::SizedDelim: return n->cls;
    case Node::Script:    return n->kids[0] ? classOf(n->kids[0]) : Ord;
    case Node::LeftRight: return Inner;
    case Node::XArrow:    return Rel;
    default:              return Ord;
    }
}

// Inter-atom spacing (TeXbook p.170). 0 none, 1 thin, 2 medium, 3 thick; +10 = only outside script styles.
const int kSpacing[8][8] = {
    /* Ord   */ { 0,  1, 12, 13,  0,  0,  0, 11},
    /* Op    */ { 1,  1,  0, 13,  0,  0,  0, 11},
    /* Bin   */ {12, 12,  0,  0, 12,  0,  0, 12},
    /* Rel   */ {13, 13,  0,  0, 13,  0,  0, 13},
    /* Open  */ { 0,  0,  0,  0,  0,  0,  0,  0},
    /* Close */ { 0,  1, 12, 13,  0,  0,  0, 11},
    /* Punct */ {11, 11,  0, 11, 11, 11, 11, 11},
    /* Inner */ {11,  1, 12, 13, 11,  0, 11, 11},
};

Box layoutSeq(const QVector<NodeP> &seq, Style st)
{
    struct Item { Box box; int cls; bool space; bool script; qreal k; };
    QVector<Item> items;
    for (const NodeP &n : seq) {
        if (!n) continue;
        if (n->kind == Node::StyleSw) { st.level = n->level; continue; }
        if (n->kind == Node::Color && n->kids.isEmpty()) { if (n->color.isValid()) st.color = n->color; continue; }
        Item it;
        it.box = layout(n, st);
        it.cls = classOf(n);
        it.space = n->kind == Node::Space;
        it.script = st.script();
        it.k = st.k();
        items.append(it);
    }
    // Binary operators without a left/right operand turn into ordinary symbols (unary minus).
    int prev = -1;
    for (Item &it : items) {
        if (it.space) continue;
        if (it.cls == Bin && (prev < 0 || prev == Bin || prev == Op || prev == Rel || prev == Open || prev == Punct)) it.cls = Ord;
        prev = it.cls;
    }
    for (int i = 0; i < items.size(); ++i) {
        if (items[i].space || items[i].cls != Bin) continue;
        int j = i + 1;
        while (j < items.size() && items[j].space) ++j;
        if (j >= items.size() || items[j].cls == Rel || items[j].cls == Close || items[j].cls == Punct) items[i].cls = Ord;
    }

    QVector<Box> out;
    int lastCls = -1;
    for (const Item &it : items) {
        if (!it.space) {
            if (lastCls >= 0) {
                int code = kSpacing[lastCls][it.cls];
                if (code >= 10) code = it.script ? 0 : code - 10;
                static const qreal amount[4] = {0, 167, 222, 278};
                if (code > 0) out << spaceBox(amount[code] * it.k);
            }
            lastCls = it.cls;
        }
        out << it.box;
    }
    return hcat(out);
}

// ---- individual constructs ------------------------------------------------------------------------
Box layoutBigOp(const NodeP &n, const Style &st)
{
    const uint cp = n->cps[0];
    quint32 g = F().glyph(cp);
    if (st.display()) {
        if (const MGConstruction *c = vertConstruction(cp)) {
            for (int i = 0; i < c->varCount; ++i) {
                g = kVertVariants[c->varOff + i].glyph;
                if (kVertVariants[c->varOff + i].size >= C::DisplayOpMinHeight) break;
            }
        }
    }
    return glyphIndexBox(g, st, true);
}

Box layoutFrac(const NodeP &n, const Style &st)
{
    Style cs = st;
    if (n->level == 0) cs.level = 0; else if (n->level == 1) cs.level = 1;
    const bool disp = cs.display();
    const qreal k = cs.k();
    const Box num = layout(n->kids[0], cs.numer());
    const Box den = layout(n->kids[1], cs.denom());

    qreal th = n->flag ? 0 : C::RuleThickness * k;
    qreal u, v;
    if (!n->flag) {
        u = (disp ? C::FracNumUpD : C::FracNumUp) * k;
        v = (disp ? C::FracDenDownD : C::FracDenDown) * k;
        const qreal gap = (disp ? C::FracNumGapD : C::FracNumGap) * k;
        const qreal axis = C::Axis * k;
        qreal clr = (u - num.desc) - (axis + th / 2);
        if (clr < gap) u += gap - clr;
        clr = (axis - th / 2) - (den.asc - v);
        if (clr < gap) v += gap - clr;
    } else {
        u = (disp ? C::StackTopUpD : C::StackTopUp) * k;
        v = (disp ? C::StackBottomDownD : C::StackBottomDown) * k;
        const qreal gap = (disp ? C::StackGapD : C::StackGap) * k;
        const qreal clr = (u - num.desc) - (den.asc - v);
        if (clr < gap) { const qreal d = (gap - clr) / 2; u += d; v += d; }
    }
    const qreal pad = 120 * k;
    const qreal w = qMax(num.w, den.w);
    Box b;
    b.w = w + 2 * pad; b.asc = u + num.asc; b.desc = v + den.desc;
    const qreal axisY = C::Axis * k;
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        num.draw ? num.draw(p, px + pad + (w - num.w) / 2, py - u) : void();
        den.draw ? den.draw(p, px + pad + (w - den.w) / 2, py + v) : void();
        if (th > 0) p.fillRect(QRectF(px + pad * 0.5, py - axisY - th / 2, w + pad, th), col);
    };
    return b;
}

Box layoutSqrt(const NodeP &n, const Style &st)
{
    MathFont &f = F();
    const qreal k = st.k();
    const Box body = layout(n->kids[0], st.crampedStyle());
    const qreal th = C::RuleThickness * k;
    const qreal gap = (st.display() ? C::RadicalGapD : C::RadicalGap) * k;
    const qreal needed = body.asc + body.desc + gap + th;
    const qreal yTop = -(body.asc + gap + th);                  // y (down) of the rule's top edge

    // choose a radical glyph (or assembly) that is tall enough
    const uint cp = 0x221A;
    const MGConstruction *c = vertConstruction(cp);
    quint32 glyph = f.glyph(cp);
    bool useAsm = false;
    if (c) {
        bool found = false;
        for (int i = 0; i < c->varCount && !found; ++i) {
            glyph = kVertVariants[c->varOff + i].glyph;
            if (kVertVariants[c->varOff + i].size * k >= needed) found = true;
        }
        if (!found && c->partCount > 0) useAsm = true;
    }
    QVector<QPair<quint32, qreal>> placed;     // glyph, origin offset (y down, absolute)
    qreal inkRight = 0, radDesc = 0, radW = 0;
    if (!useAsm) {
        const QRectF r = f.ink(glyph);
        const qreal dy = yTop - r.top() * k;
        placed.append({glyph, dy});
        inkRight = r.right() * k; radDesc = dy + r.bottom() * k; radW = f.advance(glyph) * k;
    } else {
        const AsmResult a = layoutAssembly(&kVertParts[c->partOff], c->partCount, needed / k);
        const qreal bottomUp = -yTop / k - a.total;            // units, y up
        for (const AsmPart &p : a.parts) {
            const QRectF r = f.ink(p.glyph);
            placed.append({p.glyph, -(bottomUp + p.pos + r.bottom()) * k});
            inkRight = qMax(inkRight, r.right() * k);
        }
        radDesc = -bottomUp * k; radW = inkRight;
    }
    Q_UNUSED(radW);

    // optional index  \sqrt[n]{x}
    Box deg; qreal degX = 0, degRaise = 0, shiftX = 0;
    const bool hasDeg = n->kids.size() > 1;
    if (hasDeg) {
        Style ds = st; ds.level = 3;
        deg = layout(n->kids[1], ds);
        const qreal radHeight = body.asc + gap + th + qMax(radDesc, body.desc);
        degRaise = C::RadicalRaise * radHeight - qMax(radDesc, body.desc);
        degX = C::RadicalKernBefore * k;
        const qreal radX = degX + deg.w + C::RadicalKernAfter * k;
        if (radX < 0) { shiftX = -radX; }
    }
    const qreal radX = hasDeg ? qMax<qreal>(0, degX + deg.w + C::RadicalKernAfter * k) : 0;
    const qreal bodyX = radX + inkRight;

    Box b;
    b.w = bodyX + body.w + 0.5 * th;
    b.asc = qMax(body.asc + gap + th + C::RadicalExtra * k, hasDeg ? degRaise + deg.asc : 0.0);
    b.desc = qMax(body.desc, radDesc);
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        for (const auto &pg : placed) {
            p.save(); p.translate(px + radX, py + pg.second); p.scale(k, k);
            p.fillPath(F().path(pg.first), col); p.restore();
        }
        p.fillRect(QRectF(px + radX + inkRight - 0.5 * th, py + yTop, body.w + th, th), col);
        if (body.draw) body.draw(p, px + bodyX, py);
        if (hasDeg && deg.draw) deg.draw(p, px + degX + shiftX * 0, py - degRaise);
    };
    return b;
}

bool wantsLimits(const NodeP &base, const Style &st)
{
    NodeP b = base;
    while (b && b->kind == Node::Group && b->kids.size() == 1 && b->limits == 0) b = b->kids[0];
    if (!b) return false;
    if (b->kind == Node::Brace || b->kind == Node::Stack) return true;
    if (b->limits == 1) return true;
    if (b->limits == 2) return st.display();
    return false;
}

Box layoutLimits(const Box &base, const Box *sub, const Box *sup, const Style &st)
{
    const qreal k = st.k();
    const qreal w = qMax(base.w, qMax(sub ? sub->w : 0, sup ? sup->w : 0));
    const qreal delta = base.ic;
    qreal up = 0, down = 0;
    Box b;
    b.w = w; b.asc = base.asc; b.desc = base.desc;
    if (sup) {
        up = base.asc + qMax(C::UpperLimitRiseMin * k, C::UpperLimitGapMin * k + sup->desc);
        b.asc = qMax(b.asc, up + sup->asc);
    }
    if (sub) {
        down = base.desc + qMax(C::LowerLimitDropMin * k * 0.5, C::LowerLimitGapMin * k + sub->asc);
        b.desc = qMax(b.desc, down + sub->desc);
    }
    const Box B = base; const Box *S = sub; const Box *U = sup;
    Box sb, ub; if (S) sb = *S; if (U) ub = *U;
    const bool hasS = sub != nullptr, hasU = sup != nullptr;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (B.draw) B.draw(p, px + (w - B.w) / 2, py);
        if (hasU && ub.draw) ub.draw(p, px + (w - ub.w) / 2 + delta / 2, py - up);
        if (hasS && sb.draw) sb.draw(p, px + (w - sb.w) / 2 - delta / 2, py + down);
    };
    return b;
}

Box layoutScript(const NodeP &n, const Style &st)
{
    const NodeP &baseN = n->kids[0];
    const Box base = layout(baseN, st);
    Box subB, supB;
    const bool hasSub = n->kids[1] != nullptr, hasSup = n->kids[2] != nullptr;
    if (hasSub) subB = layout(n->kids[1], st.sub());
    if (hasSup) supB = layout(n->kids[2], st.sup());

    if (wantsLimits(baseN, st)) return layoutLimits(base, hasSub ? &subB : nullptr, hasSup ? &supB : nullptr, st);

    const qreal k = st.k(), ks = st.sup().k();
    const bool isChar = baseN->kind == Node::Atom && !baseN->bigOp && base.glyphs <= 1;
    qreal u = 0, v = 0;
    if (!isChar) { u = base.asc - C::SupBaselineDropMax * ks; v = base.desc + C::SubBaselineDropMin * ks; }

    if (hasSup) {
        u = qMax(u, (st.cramped ? C::SupShiftUpCramped : C::SupShiftUp) * k);
        u = qMax(u, supB.desc + C::SupBottomMin * k);
    }
    if (hasSub && !hasSup) {
        v = qMax(v, C::SubShiftDown * k);
        v = qMax(v, subB.asc - C::SubTopMax * k);
    }
    if (hasSub && hasSup) {
        v = qMax(v, C::SubShiftDown * k);
        const qreal gap = (u - supB.desc) - (subB.asc - v);
        if (gap < C::SubSupGapMin * k) {
            v += C::SubSupGapMin * k - gap;
            const qreal psi = C::SupBottomMaxWithSub * k - (u - supB.desc);
            if (psi > 0) { u += psi; v -= psi; }
        }
    }
    const qreal supX = base.w + base.ic;
    const qreal subX = base.w - (baseN->kind == Node::Atom && baseN->bigOp ? base.ic : 0);
    const qreal after = C::SpaceAfterScript * k;
    Box b;
    b.w = qMax(hasSup ? supX + supB.w : 0.0, hasSub ? subX + subB.w : 0.0) + after;
    b.asc = qMax(base.asc, hasSup ? u + supB.asc : 0.0);
    b.desc = qMax(base.desc, hasSub ? v + subB.desc : 0.0);
    b.glyphs = 2;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (base.draw) base.draw(p, px, py);
        if (hasSup && supB.draw) supB.draw(p, px + supX, py - u);
        if (hasSub && subB.draw) subB.draw(p, px + subX, py + v);
    };
    return b;
}

Box delimiterBox(uint cp, qreal target, const Style &st)
{
    if (cp == 0) return Box();
    return stretchyVertical(cp, target, st);
}

Box layoutLeftRight(const NodeP &n, const Style &st)
{
    QVector<Box> segs;
    for (const NodeP &kid : n->kids) segs << layout(kid, st);
    const qreal k = st.k(), axis = C::Axis * k;
    qreal a = 0;
    for (const Box &s : segs) a = qMax(a, qMax(s.asc - axis, s.desc + axis));
    const qreal H = 2 * a;
    const qreal target = qMax(H * 0.901, H - 500 * k) / k;       // TeX: delimiterfactor 901, delimitershortfall 5pt

    QVector<Box> parts;
    for (int i = 0; i < n->delims.size(); ++i) {
        parts << delimiterBox(n->delims[i].toUInt(), target, st);
        if (i < segs.size()) parts << segs[i];
    }
    return hcat(parts);
}

Box layoutMatrix(const NodeP &n, const Style &st)
{
    const QString env = n->env;
    const bool alignEnv = env == "aligned" || env == "align" || env == "align*" || env == "split" || env == "alignat" || env == "alignat*" ||
                          env == "eqnarray" || env == "flalign" || env == "alignedat";
    const bool gatherEnv = env == "gather" || env == "gather*" || env == "gathered" || env == "equation" || env == "equation*" || env == "multline" || env == "multline*";
    const bool small = env == "smallmatrix" || env == "substack" || env == "subarray";

    Style cs = st;
    if (alignEnv || gatherEnv) cs.level = 0;
    else if (small) cs.level = 2;
    else cs.level = qMin(st.level, 1) == 0 ? 1 : st.level;     // matrices use text style
    const qreal k = cs.k();

    int ncols = 0;
    for (const auto &row : n->grid) ncols = qMax(ncols, int(row.size()));
    QVector<QChar> align(ncols, 'c');
    if (env == "cases" || env == "dcases") align.fill('l');
    else if (alignEnv) for (int j = 0; j < ncols; ++j) align[j] = (j % 2 == 0) ? 'r' : 'l';
    else if (env == "array" || env == "alignat") {
        int j = 0;
        for (const QChar &ch : n->colSpec) if ((ch == 'l' || ch == 'c' || ch == 'r') && j < ncols) align[j++] = ch;
    }
    const qreal rowGap = (alignEnv || gatherEnv ? 500 : (small ? 100 : 300)) * k;
    QVector<qreal> gapAfter(ncols, 0);            // gap between column j and j+1 (precomputed: the draw lambda outlives this scope)
    for (int j = 0; j + 1 < ncols; ++j)
        gapAfter[j] = alignEnv ? ((j % 2 == 0) ? 0 : 2000 * k) : (small ? 600 : 1000) * k;

    const int nrows = n->grid.size();
    QVector<QVector<Box>> cells(nrows);
    QVector<qreal> colW(ncols, 0), rowA(nrows, 0), rowD(nrows, 0);
    for (int r = 0; r < nrows; ++r) {
        rowA[r] = (small ? 450 : 700) * k; rowD[r] = (small ? 150 : 300) * k;
        for (int c = 0; c < ncols; ++c) {
            Box b;
            if (c < n->grid[r].size() && n->grid[r][c]) {
                QVector<NodeP> items = n->grid[r][c]->kids;
                if (alignEnv && c % 2 == 1) items.prepend(mk(Node::Group));     // empty Ord => proper space before "="
                b = layoutSeq(items, cs);
            }
            cells[r].append(b);
            colW[c] = qMax(colW[c], b.w);
            rowA[r] = qMax(rowA[r], b.asc);
            rowD[r] = qMax(rowD[r], b.desc);
        }
    }
    qreal W = 0, H = 0;
    for (int c = 0; c < ncols; ++c) W += colW[c] + gapAfter[c];
    for (int r = 0; r < nrows; ++r) H += rowA[r] + rowD[r] + (r + 1 < nrows ? rowGap : 0);

    Box b;
    b.w = W; b.asc = H / 2 + C::Axis * k; b.desc = H / 2 - C::Axis * k;
    const qreal top = b.asc;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        qreal y = -top;
        for (int r = 0; r < nrows; ++r) {
            y += rowA[r];
            qreal x = 0;
            for (int c = 0; c < ncols; ++c) {
                const Box &cb = cells[r][c];
                qreal off = align[c] == 'l' ? 0 : (align[c] == 'r' ? colW[c] - cb.w : (colW[c] - cb.w) / 2);
                if (cb.draw) cb.draw(p, px + x + off, py + y);
                x += colW[c] + gapAfter[c];
            }
            y += rowD[r] + rowGap;
        }
    };
    return b;
}

Box layoutAccent(const NodeP &n, const Style &st)
{
    MathFont &f = F();
    const qreal k = st.k();
    const Box body = layout(n->kids[0], st.crampedStyle());
    const uint cp = n->cps[0];
    quint32 g = f.glyph(cp);
    if (n->flag && body.glyphs != 1) {                              // wide accent: pick a variant that spans the body
        if (const MGConstruction *c = horizConstruction(cp)) {
            for (int i = 0; i < c->varCount; ++i) {
                g = kHorizVariants[c->varOff + i].glyph;
                if (kHorizVariants[c->varOff + i].size * k >= body.w) break;
            }
        }
    }
    const QRectF r = f.ink(g);
    int ax = 0;
    const qreal accentAttach = lookupValue(kTopAccent, kTopAccentCount, g, &ax) ? ax : (r.left() + r.right()) / 2;
    const qreal baseAttach = body.accentX >= 0 ? body.accentX : body.w / 2;
    const qreal shiftUp = qMax<qreal>(0, body.asc - C::AccentBaseHeight * k);
    const qreal ox = baseAttach - accentAttach * k;

    Box b;
    b.w = body.w; b.desc = body.desc; b.ic = body.ic;
    b.asc = qMax(body.asc, -r.top() * k + shiftUp);
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (body.draw) body.draw(p, px, py);
        p.save(); p.translate(px + ox, py - shiftUp); p.scale(k, k);
        p.fillPath(F().path(g), col); p.restore();
    };
    return b;
}

Box layoutLine(const NodeP &n, const Style &st)           // \overline / \underline
{
    const qreal k = st.k();
    const Box body = layout(n->kids[0], st.crampedStyle());
    const qreal th = C::RuleThickness * k, gap = 3 * th;
    Box b = body;
    const bool under = n->flag;
    if (under) b.desc = body.desc + gap + th; else b.asc = body.asc + gap + th;
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (body.draw) body.draw(p, px, py);
        const qreal y = under ? py + body.desc + gap : py - body.asc - gap - th;
        p.fillRect(QRectF(px, y, body.w, th), col);
    };
    return b;
}

Box layoutBrace(const NodeP &n, const Style &st)           // \overbrace / \underbrace
{
    MathFont &f = F();
    const qreal k = st.k();
    const Box body = layout(n->kids[0], st);
    const bool under = n->flag;
    const uint cp = under ? 0x23DF : 0x23DE;
    const qreal gap = 100 * k;
    const MGConstruction *c = horizConstruction(cp);
    const qreal target = body.w / k;

    struct P { quint32 g; qreal x; };
    QVector<P> ps;
    qreal inkTop = 0, inkBottom = 0, width = body.w;
    quint32 base = f.glyph(cp);
    bool done = false;
    if (c) {
        for (int i = 0; i < c->varCount && !done; ++i)
            if (kHorizVariants[c->varOff + i].size >= target) { base = kHorizVariants[c->varOff + i].glyph; done = true; }
        if (!done && c->partCount > 0) {
            const AsmResult a = layoutAssembly(&kHorizParts[c->partOff], c->partCount, target);
            for (const AsmPart &p : a.parts) ps.append({p.glyph, p.pos});
            width = qMax(width, a.total * k);
            done = true;
        } else if (!done && c->varCount > 0) base = kHorizVariants[c->varOff + c->varCount - 1].glyph;
    }
    if (ps.isEmpty()) { ps.append({base, 0}); width = qMax(width, f.advance(base) * k); }
    inkTop = 1e9; inkBottom = -1e9;
    for (const P &p : ps) { const QRectF r = f.ink(p.g); inkTop = qMin(inkTop, r.top()); inkBottom = qMax(inkBottom, r.bottom()); }

    // position the brace glyph so that its ink touches `gap` above/below the body
    const qreal braceH = (inkBottom - inkTop) * k;
    qreal originY;                                      // y (down) of the glyph origin
    if (!under) originY = -(body.asc + gap) - inkBottom * k;
    else        originY = body.desc + gap - inkTop * k;
    const qreal bw = ps.last().x + f.advance(ps.last().g);
    const qreal braceW = qMax(bw * k, 0.0);
    const qreal W = qMax(body.w, braceW);

    Box b;
    b.w = W; b.asc = body.asc; b.desc = body.desc;
    if (!under) b.asc = body.asc + gap + braceH; else b.desc = body.desc + gap + braceH;
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (body.draw) body.draw(p, px + (W - body.w) / 2, py);
        const qreal ox = px + (W - braceW) / 2;
        for (const P &q : ps) {
            p.save(); p.translate(ox + q.x * k, py + originY); p.scale(k, k);
            p.fillPath(F().path(q.g), col); p.restore();
        }
    };
    return b;
}

Box layoutStack(const NodeP &n, const Style &st)           // \overset / \underset
{
    const Box base = layout(n->kids[0], st);
    Box label;
    const bool under = n->flag;
    label = layout(under ? n->kids[2] : n->kids[1], st.sup());
    return layoutLimits(base, under ? &label : nullptr, under ? nullptr : &label, st);
}

Box layoutXArrow(const NodeP &n, const Style &st)           // \xrightarrow[below]{above}
{
    const qreal k = st.k();
    const Style ss = st.sup();
    const Box above = layout(n->kids[0], ss);
    Box below; const bool hasBelow = n->kids.size() > 1 && n->kids[1];
    if (hasBelow) below = layout(n->kids[1], ss.sub());
    const qreal w = qMax(qMax(above.w, below.w) + 800 * k, 1500 * k);
    const qreal axis = C::Axis * k, th = C::RuleThickness * k;
    const bool left = n->env.contains("left") && !n->env.contains("right");
    const bool both = n->env.contains("leftright");
    const bool dbl = n->env.contains("Right") || n->env.contains("Left");
    const qreal head = 150 * k;
    const QColor col = st.color;

    Box b;
    b.w = w;
    b.asc = axis + 60 * k + above.desc + 120 * k + above.asc;
    b.desc = hasBelow ? qMax<qreal>(0, -axis + 60 * k) + 120 * k + below.asc + below.desc : 0;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        const qreal y = py - axis;
        p.save();
        QPen pen(col, th, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        const qreal off = dbl ? th * 1.5 : 0;
        p.drawLine(QPointF(px + 40 * k, y - off), QPointF(px + w - 40 * k, y - off));
        if (dbl) p.drawLine(QPointF(px + 40 * k, y + off), QPointF(px + w - 40 * k, y + off));
        auto headAt = [&](qreal x, int dir) {
            p.drawLine(QPointF(x, y), QPointF(x - dir * head, y - head * 0.9));
            p.drawLine(QPointF(x, y), QPointF(x - dir * head, y + head * 0.9));
        };
        if (!left || both) headAt(px + w - 40 * k, 1);
        if (left || both)  headAt(px + 40 * k, -1);
        p.restore();
        if (above.draw) above.draw(p, px + (w - above.w) / 2, py - axis - 60 * k - 120 * k - above.desc);
        if (hasBelow && below.draw) below.draw(p, px + (w - below.w) / 2, py - axis + 60 * k + 120 * k + below.asc);
    };
    return b;
}

Box layoutBoxed(const NodeP &n, const Style &st)
{
    const qreal k = st.k();
    const Box body = layout(n->kids[0], st);
    const qreal pad = 150 * k, th = C::RuleThickness * k;
    Box b;
    b.w = body.w + 2 * pad; b.asc = body.asc + pad; b.desc = body.desc + pad;
    const QColor col = st.color;
    b.draw = [=](QPainter &p, qreal px, qreal py) {
        if (body.draw) body.draw(p, px + pad, py);
        p.save(); p.setPen(QPen(col, th)); p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(px + th / 2, py - b.asc + th / 2, b.w - th, b.asc + b.desc - th));
        p.restore();
    };
    return b;
}

Box layout(const NodeP &n, const Style &stIn)
{
    if (!n) return Box();
    Style st = stIn;
    switch (n->kind) {
    case Node::Atom:       return n->bigOp ? layoutBigOp(n, st) : glyphBox(n->cps, st);
    case Node::Text: {
        if (n->error) st.color = QColor("#d32f2f");
        return glyphBox(n->cps, st);
    }
    case Node::Group:      return layoutSeq(n->kids, st);
    case Node::Space:      return spaceBox(n->num * st.k());
    case Node::Frac:       return layoutFrac(n, st);
    case Node::Sqrt:       return layoutSqrt(n, st);
    case Node::Script:     return layoutScript(n, st);
    case Node::LeftRight:  return layoutLeftRight(n, st);
    case Node::Matrix:     return layoutMatrix(n, st);
    case Node::Accent:     return layoutAccent(n, st);
    case Node::Line:       return layoutLine(n, st);
    case Node::Brace:      return layoutBrace(n, st);
    case Node::Stack:      return layoutStack(n, st);
    case Node::XArrow:     return layoutXArrow(n, st);
    case Node::Boxed:      return layoutBoxed(n, st);
    case Node::SizedDelim: return stretchyVertical(n->cps[0], n->num, st);
    case Node::Phantom: {
        Box b = layout(n->kids[0], st);
        b.draw = nullptr;
        if (n->phantomKind == 1) { b.asc = b.desc = 0; }
        if (n->phantomKind == 2) { b.w = 0; }
        return b;
    }
    case Node::Color: {
        if (n->kids.isEmpty()) return Box();
        if (n->color.isValid()) st.color = n->color;
        return layout(n->kids[0], st);
    }
    case Node::StyleSw:    return Box();
    }
    return Box();
}

} // namespace

// =============================================================================
//  4. Public API
// =============================================================================
namespace MathRenderer {

static QHash<QString, MathImage> &cache() { static QHash<QString, MathImage> c; return c; }

MathImage render(const QString &tex, bool display, const MathOptions &opt)
{
    const QString key = QString("%1|%2|%3|%4|%5|%6").arg(tex).arg(display).arg(opt.color.name())
                            .arg(opt.emPx, 0, 'f', 2).arg(opt.scale, 0, 'f', 3).arg(opt.dpr, 0, 'f', 1);
    if (auto it = cache().find(key); it != cache().end()) return it.value();

    MathImage out;
    if (!F().ok()) { out.error = "Math font could not be loaded"; return out; }

    Parser parser(tex);
    const NodeP root = parser.parseAll();
    Style st;
    st.level = display ? 0 : 1;
    st.color = opt.color;
    const Box b = layout(root, st);

    const qreal s = opt.emPx * opt.scale / 1000.0;                 // font units -> logical pixels
    const qreal padX = qMax(1.0, 0.05 * opt.emPx * opt.scale), padY = 1.0;
    const qreal ascPx = qMax<qreal>(0, b.asc) * s, descPx = qMax<qreal>(0, b.desc) * s;
    const qreal Wl = b.w * s + 2 * padX, Hl = ascPx + descPx + 2 * padY;

    QImage img(QSize(qMax(1, int(std::ceil(Wl * opt.dpr))), qMax(1, int(std::ceil(Hl * opt.dpr)))), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(opt.dpr);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        p.translate(padX, padY + ascPx);
        p.scale(s, s);
        if (b.draw) b.draw(p, 0, 0);
    }
    out.image   = img;
    out.width   = img.width() / opt.dpr;
    out.height  = img.height() / opt.dpr;
    out.ascent  = padY + ascPx;
    out.descent = out.height - out.ascent;
    out.ok      = true;

    if (cache().size() > 600) cache().clear();
    cache().insert(key, out);
    return out;
}

// ---- Markdown extraction ----------------------------------------------------------------------------
namespace {

QString token(QVector<MathItem> &items, const QString &tex, bool display)
{
    items.append({tex.trimmed(), display});
    return QString("ZZMATH%1%2ZZ").arg(display ? 'D' : 'I').arg(items.size() - 1);
}

QString scanInline(const QString &s, QVector<MathItem> &items)
{
    QString out;
    const int n = s.size();
    int i = 0;
    while (i < n) {
        const QChar c = s[i];
        if (c == '\\' && i + 1 < n) {
            if (s[i + 1] == '(') {
                const int e = s.indexOf("\\)", i + 2);
                if (e > i + 2) { out += token(items, s.mid(i + 2, e - i - 2), false); i = e + 2; continue; }
            }
            out += c; out += s[i + 1]; i += 2;              // keep "\$" and other escapes untouched
            continue;
        }
        if (c == '`') {                                      // code span: copy verbatim
            int run = 0;
            while (i + run < n && s[i + run] == '`') ++run;
            const QString tick(run, '`');
            const int e = s.indexOf(tick, i + run);
            if (e < 0) { out += tick; i += run; continue; }
            out += s.mid(i, e + run - i);
            i = e + run;
            continue;
        }
        if (c == '$') {
            if (i + 1 < n && s[i + 1] == '$') {              // $$ ... $$ inside a line => display-style inline
                const int e = s.indexOf("$$", i + 2);
                if (e > i + 2) { out += token(items, s.mid(i + 2, e - i - 2), true); i = e + 2; continue; }
                out += "$$"; i += 2; continue;
            }
            // single $: opening must be followed by non-space; closing preceded by non-space and not followed by a digit
            if (i + 1 < n && !s[i + 1].isSpace()) {
                int j = i + 1;
                bool found = false;
                while (j < n) {
                    if (s[j] == '\\') { j += 2; continue; }
                    if (s[j] == '`') {                          // never close inside (or across) a code span
                        int run = 0;
                        while (j + run < n && s[j + run] == '`') ++run;
                        const int e = s.indexOf(QString(run, '`'), j + run);
                        j = e < 0 ? j + run : e + run;
                        continue;
                    }
                    if (s[j] == '$') { found = !s[j - 1].isSpace() && (j + 1 >= n || !s[j + 1].isLetterOrNumber()); break; }
                    ++j;
                }
                if (found && j > i + 1) { out += token(items, s.mid(i + 1, j - i - 1), false); i = j + 1; continue; }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

} // namespace

QString extractMath(const QString &md, QVector<MathItem> &items)
{
    if (!md.contains('$') && !md.contains("\\(") && !md.contains("\\[") && !md.contains("```math")) return md;

    static const QRegularExpression fenceRe(R"(^(\s*)(`{3,}|~{3,})\s*([A-Za-z0-9_+-]*))");
    const QStringList lines = md.split('\n');
    QStringList out;
    bool inFence = false;
    QString fence;

    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines[i];
        const auto fm = fenceRe.match(line);
        if (fm.hasMatch()) {
            const QString f = fm.captured(2);
            if (!inFence) {
                if (fm.captured(3).toLower() == "math") {              // ```math ... ```  (GitHub style)
                    QStringList body;
                    int j = i + 1;
                    for (; j < lines.size(); ++j) {
                        const auto cm = fenceRe.match(lines[j]);
                        if (cm.hasMatch() && cm.captured(2)[0] == f[0] && cm.captured(2).size() >= f.size() && cm.captured(3).isEmpty()) break;
                        body << lines[j];
                    }
                    out << QString() << fm.captured(1) + token(items, body.join('\n'), true) << QString();
                    i = j;
                    continue;
                }
                inFence = true; fence = f;
            } else if (f[0] == fence[0] && f.size() >= fence.size()) {
                inFence = false;
            }
            out << line;
            continue;
        }
        if (inFence) { out << line; continue; }

        // display math that owns its lines:  $$ ... $$   or   \[ ... \]
        const QString t = line.trimmed();
        if (t.startsWith("$$") || t.startsWith("\\[")) {
            const bool dollar = t.startsWith("$$");
            const QString close = dollar ? "$$" : "\\]";
            const QString indent = line.left(line.size() - line.trimmed().size() - (line.size() - line.trimmed().size() - (line.size() - line.indexOf(t))) );
            const QString lead = line.left(line.indexOf(t));
            const QString rest = t.mid(2);
            const int ci = rest.indexOf(close);
            if (ci >= 0) {
                if (rest.mid(ci + 2).trimmed().isEmpty()) {
                    out << QString() << lead + token(items, rest.left(ci), true) << QString();
                    continue;
                }
            } else {
                QStringList body{rest};
                bool found = false;
                int j = i + 1;
                for (; j < lines.size() && j < i + 400; ++j) {
                    const int c2 = lines[j].indexOf(close);
                    if (c2 >= 0) { body << lines[j].left(c2); found = lines[j].mid(c2 + 2).trimmed().isEmpty(); break; }
                    body << lines[j];
                }
                if (found) {
                    out << QString() << lead + token(items, body.join('\n'), true) << QString();
                    i = j;
                    continue;
                }
            }
            Q_UNUSED(indent);
        }
        out << scanInline(line, items);
    }
    return out.join('\n');
}

// ---- Placeholder replacement ----------------------------------------------------------------------------
namespace {

// Qt aligns an inline image with AlignMiddle by centring it inside the line box:
//   image ascent = (H + halfX) / 2, descent = (H - halfX) / 2   (halfX = xHeight / 2)
//   line ascent/descent = max(font ascent/descent, image ascent/descent)
//   image centre = baseline - (lineAscent - lineDescent) / 2
// We therefore pad the formula image (transparent rows above/below) so that the formula's own
// baseline lands exactly on the text baseline.  Returns the padded image and its descent.
QImage padForBaseline(const MathImage &m, const QFont &font, qreal dpr, qreal *outDescent, qreal *outHeight)
{
    const QFontMetricsF fm(font);
    const qreal fa = fm.ascent(), fd = fm.descent();
    const qreal hx = QFontMetrics(font).xHeight() / 2.0;
    const qreal A = m.ascent, D = m.descent;

    auto centreAboveBaseline = [&](qreal H) {
        const qreal ia = (H + hx) / 2, id = (H - hx) / 2;
        return (qMax(fa, ia) - qMax(fd, id)) / 2;
    };
    qreal H = A + D;
    for (; H < 6 * (A + D) + 80; H += 0.25) {
        const qreal r = H / 2 + centreAboveBaseline(H);              // baseline row from the top
        if (r >= A - 0.01 && H - r >= D - 0.01) break;
    }
    const qreal r = H / 2 + centreAboveBaseline(H);
    const qreal topPad = qMax<qreal>(0, r - A);

    QImage out(QSize(m.image.width(), qMax(1, int(std::ceil(H * dpr)))), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.drawImage(QPointF(0, topPad), m.image);
    }
    *outHeight = out.height() / dpr;
    *outDescent = *outHeight - r;
    return out;
}

} // namespace

void applyMath(QTextDocument *doc, const QVector<MathItem> &items, const MathOptions &opt)
{
    static const QRegularExpression re("ZZMATH([ID])(\\d+)ZZ");
    int pos = 0;
    while (true) {
        QTextCursor cur = doc->find(re, pos);
        if (cur.isNull()) break;
        const auto m = re.match(cur.selectedText());
        const int idx = m.captured(2).toInt();
        if (idx < 0 || idx >= items.size()) { pos = cur.position(); continue; }
        const MathItem &it = items[idx];

        // size the formula to the text around it (so formulas inside headings scale with them)
        QTextCursor probe(doc);
        probe.setPosition(cur.selectionStart() + 1);
        const QTextCharFormat cf = probe.charFormat();
        const QFont font = cf.font().resolve(doc->defaultFont());     // same resolution the text layout uses
        MathOptions local = opt;
        const int px = QFontInfo(font).pixelSize();
        if (px > 0) local.emPx = px;

        const MathImage img = render(it.tex, it.display, local);
        cur.removeSelectedText();
        if (!img.ok) { cur.insertText("$" + it.tex + "$"); pos = cur.position(); continue; }

        qreal descent = 0, height = 0;
        const QImage padded = padForBaseline(img, font, local.dpr, &descent, &height);

        // deterministic resource name => repeated refreshes reuse the same entry
        const QString name = QString("mdmath://%1").arg(qHash(QString("%1|%2|%3|%4|%5|%6").arg(it.tex).arg(it.display)
                                 .arg(local.color.name()).arg(local.emPx).arg(local.scale).arg(font.key())), 0, 16);
        doc->addResource(QTextDocument::ImageResource, QUrl(name), padded);

        QTextImageFormat fmt;
        fmt.merge(cf);                                      // keep colour/size of the surrounding text
        fmt.setFont(font);                                  // Qt takes xHeight from THIS font when aligning (see padForBaseline)
        fmt.setName(name);
        fmt.setWidth(img.width);
        fmt.setHeight(height);
        fmt.setProperty(DescentProperty, descent);
        fmt.setProperty(TexProperty, it.tex);
        fmt.setProperty(DisplayProperty, it.display);
        fmt.setVerticalAlignment(QTextCharFormat::AlignMiddle);
        cur.insertImage(fmt);
        pos = cur.position();

        // a display formula that stands alone in its paragraph is centred
        if (it.display && cur.block().length() <= 2) {
            QTextBlockFormat bf = cur.blockFormat();
            bf.setAlignment(Qt::AlignHCenter);
            cur.setBlockFormat(bf);
        }
    }
}

} // namespace MathRenderer
