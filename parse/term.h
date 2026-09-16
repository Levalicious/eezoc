typedef enum {VARIABLE, ABSTRACTION, APPLICATION, NUMERAL, OPERATION} TermType;

// names in Operations must line up with codes in OperationCode
static const char* const Operations[] = {"", "+", "--", "*", "//", "%",
    "=", "=/=", "<", ">", "<=", ">=", "abort",
    "up", "(exit)", "(put)", "(get)",
    /* the word primitives (2026-09-13), in the order of libeezo's PrimOp: reserved names */
    "wadd", "wsub", "wmul", "wand", "wor", "wxor", "wshl", "wshr",
    "weq", "wlt", "waddc", "wsubb", "wmull", "wdivmod",
    /* the limb primitives (2026-09-16), likewise in PrimOp order: they take limb lists (a machine
       word is one), and the C list of limbs evaluates them directly */
    "badd", "bsub", "bmul", "bdivmod", "blt", "beq",
    "bpow", "bminv"};
typedef enum {NONE, PLUS, MONUS, TIMES, DIVIDE, MODULO, EQUAL, NOTEQUAL,
      LESSTHAN, GREATERTHAN, LESSTHANOREQUAL, GREATERTHANOREQUAL,
      ABORT, INCREMENT, EXIT, PUT, GET,
      WADD, WSUB, WMUL, WAND, WOR, WXOR, WSHL, WSHR,
      WEQ, WLT, WADDC, WSUBB, WMULL, WDIVMOD,
      BADD, BSUB, BMUL, BDIVMOD, BLT, BEQ, BPOW, BMINV} OperationCode;

static inline bool isPseudoOperation(OperationCode c) {
    return c == ABORT || c == EXIT || c == PUT || c == GET || (c >= WADD && c <= BMINV);
}

typedef Node Term;
static inline TermType getTermType(Term* t) {return (TermType)getType(t);}
static inline bool isVariable(Term* t) {return getType(t) == VARIABLE;}
static inline bool isAbstraction(Term* t) {return getType(t) == ABSTRACTION;}
static inline bool isApplication(Term* t) {return getType(t) == APPLICATION;}
static inline bool isNumeral(Term* t) {return getType(t) == NUMERAL;}
static inline bool isOperation(Term* t) {return getType(t) == OPERATION;}
static inline bool isGlobal(Term* t) {return isVariable(t) && getValue(t) < 0;}
static inline bool isValueType(TermType t) {
    return t == ABSTRACTION || t == NUMERAL;
}
static inline bool isValue(Term* t) {return isValueType(getTermType(t));}

static inline Term* Variable(Tag tag, long long debruijn) {
    return newLeaf(tag, VARIABLE, 0, debruijn);
}

static inline Term* Abstraction(Tag tag, Term* body) {
    return newBranch(tag, ABSTRACTION, 0, NULL, body);
}

static inline Term* Application(Tag tag, Term* left, Term* right) {
    return newBranch(tag, APPLICATION, 0, left, right);
}

static inline Term* Numeral(Tag tag, long long n) {
    return newLeaf(tag, NUMERAL, 0, n);
}

// note: arithmetic operations are branches and always have a fallback term
// but pseudo operations are leaves and don't have a fallback term
static inline Term* Operation(Tag tag, OperationCode code, Term* term) {
    return newBranch(tag, OPERATION, (char)code, NULL, term);
}

static inline unsigned long long getDebruijnIndex(Term* t) {
    assert(getValue(t) > 0);
    return (unsigned long long)getValue(t);
}

static inline Term* getGlobalReferent(Term* global, const Array* globals) {
    assert(getValue(global) < 0);
    return elementAt(globals, (size_t)(-getValue(global) - 1));
}

static inline Term* getParameter(Term* t) {return getLeft(t);}
static inline Term* getBody(Term* t) {return getRight(t);}
static inline void setBody(Term* t, Term* body) {setRight(t, body);}
static inline OperationCode getOperationCode(Term* t) {
    assert(isOperation(t));
    return (OperationCode)getVariety(t);
}
