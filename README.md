# SageTree

**A friendlier language for friendlier times.**

SageTree is a new, very new, experimental programming language. It was originally created by one person (Night-Traders-Dev as SageLang), picked up and heavily modified by another (MilkmanAbi), and is currently maintained by a single developer. Progress is slow. That is the honest reality, and we think that is okay.

Sage is not trying to be the fastest language. It is not trying to replace anything. It is trying to be **kinder**. A language that tells you what went wrong instead of crashing silently. A language that gives you choices instead of forcing a single paradigm. A language that respects your time.

```sage
let greeting = "hello, world"
println(greeting.upper())

struct Point:
    x: float
    y: float

impl Point:
    proc distance(self, other: Point) -> float:
        let dx = self.x - other.x
        let dy = self.y - other.y
        return math.sqrt(dx * dx + dy * dy)
```

(^ ~^ )


## What Sage Is

Sage can be **interpreted or compiled**. This is not a side feature. Both paths are native to the language, supported by the same toolchain, and share the same standard library.

- `sage script.sage` runs your code through MossVM (the bytecode virtual machine) or the AST interpreter
- `sage --aot script.sage` compiles to a native binary (experimental)

The interpreter is the primary path right now. The compiler is being rebuilt.


## Features

**Types that help, not hurt.**
Sage uses gradual typing. You can write `var x = 42` and Sage infers it, or `int x = 42` and Sage checks it. Type annotations are enforced at runtime. `int x = nil` is an error, not a silent bug.

```sage
var flexible = "anything goes"
int strict = 42
str name = "Abi"
```

**Memory management your way.**
Sage gives you three modes and lets you mix them.

- **GC mode** (default). SageGC handles everything. Tri-color mark-sweep, concurrent, handles cycles. You write code, it cleans up.
- **Manual mode**. `@manual:` blocks pause the GC. You get `mem_alloc`, `mem_free`, `mem_read`, `mem_write`, pointer arithmetic. Full control when you need it.
- **Hybrid mode**. `gc_disable()` and `gc_enable()` inline. Use manual allocation for the hot path, let GC handle the rest.

```sage
# GC handles this
let data = [1, 2, 3, 4, 5]

# Manual when you need it
@manual:
    var buf = mem_alloc(1024)
    mem_write(buf, 0, "int", 42)
    mem_free(buf)

# Hybrid for performance-critical sections
gc_disable()
var pool = mem_alloc(4096)
# ... hot loop ...
gc_enable()
```

**Optional memory safety.**
Sage has an ownership and borrow analysis system inspired by Rust. It is opt-in, not forced. Run `sage safety your_file.sage` and Sage will check for use-after-move, double-move, and aliased mutation. Or run `sage --strict-safety your_file.sage` to analyze then execute.

**Enums with data (ADTs).**

```sage
enum Shape:
    Circle(radius: float)
    Rect(w: float, h: float)
    Point

match shape:
    case Shape.Circle(r):
        println("radius: " + str(r))
    case Shape.Rect(w, h):
        println("area: " + str(w * h))
    case Shape.Point:
        println("just a point")
```

**Python interop, natively.**
Import any Python library and call it directly.

```sage
import python
let np = python.import("numpy")
let arr = np.array([1, 2, 3, 4, 5])
println(np.mean(arr))
```

(  o w o )


## Subsystems

SageTree is built from named subsystems. Each one does one thing.

| Name | What it does |
|------|-------------|
| **MossVM** | Bytecode virtual machine. Primary interpreter runtime. |
| **SageGC** | Tri-color mark-sweep garbage collector with concurrent marking. |
| **Firefly** | Error diagnosis. Source locations, "did you mean?", call stacks, advice. |
| **LilyBox** | Sandbox. Capability-based execution environment for untrusted code. |
| **LilyKnight** | Permission enforcement inside LilyBox. Controls fs, net, exec, FFI access. |
| **FrogPond** | User-facing sandbox API (`import sandbox`). |

Firefly is the system we are most proud of. When something goes wrong, Firefly tells you what, where, and why:

```
-- error[E001]: undefined variable 'nme' -- script.sage:2:9 --
    |
  2 | println(nme)
    |         ^^^
    |
   Firefly: 'nme' is not defined in this scope.
            Did you mean 'name'? (1 character off)

   Call stack:
     0. greet at script.sage:5
     1. main at script.sage:10
----------------------------------------------------------------------
```

Firefly has three verbosity levels: `--firefly=full` (default), `--firefly=minimal`, `--firefly=off`.


## Building

You need GCC and Make. Python 3 and libffi are optional (for FFI features).

```sh
make
./sage --info
./sage your_script.sage
```

Run the test suite:

```sh
make test
```

That is it.

( ^ v ^ )b


## Current State

SageTree is alpha software. 293 tests pass. The interpreter works. The compiler is experimental. There are bugs. The documentation is incomplete. We are one person working on this.

Things that work well: gradual typing, structs with impl blocks, ADT enums with pattern matching, closures, generators, async/await, Python FFI, the REPL, Firefly error diagnosis, the sandbox, manual memory management.

Things that need work: performance on recursive calls (we are slower than Python there), generics, traits, a package manager, IDE integration.

We are building this slowly, carefully, and honestly. If that sounds like something you want to follow along with or contribute to, you are welcome here.


## License

MIT.
