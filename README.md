# Mewo Build System

Mewo is a **lightweight** build system designed for being an **aliasing system** more than the traditional *make-like* build systems.

> [!WARNING]
> Mewo is in early stages of development. Many things are not done or don't work at all and anything can change at any moment.

## Getting started

To start with Mewo you need a `Mewofile`.

```mewo
build:
    echo Hello world
```

To run this you simply do `mewo build`.

If you want code to execute only on `mewo`, similar to `all` in Make, you can just put it to top level.

```mewo
call build

build:
    echo Hello world
```

This will run build on `mewo`.

---

One of main features of Mewo is OS awarness, which means the code will do different things on different platforms.

This code will echo different message on different oses:

```mewo
#windows echo "Hello from windows"
#linux echo "Hello from linux"
#macos echo "Hello from macos"
```

The `#attr` can be placed on any label or any single command.

```mewo
#unix build:
    gcc main.c -o main

#windows build:
    cl main.c /Fe:main.exe
```

---

Mewo also supports string interpolation, which is done thru `${}` syntax.
This works anywhere.

```mewo
i = 10
name = "World"

echo Hello ${name}

test:
    echo ${i}
```

And some functions directly inside string interpolation.

```mewo
echo File mewo is ${#sizeof(file, mewo, KiB)} KiB in size.
```

---

Comments are `;` and `//` btw

## Installation

To install Mewo, you need to first bootstrap it with nob.  

```console
gcc nob.c -o nob
./nob
```

Then, you can install it with Mewofile.

```console
./mewo release
./mewo install
```

After this you can access mewo from anywhere.

```console
mewo -v
```

### Dependencies

Mewo depends *only* on libc.

Build-time dependencies:

#### Windows

I recommend these from [MSYS2 MinGW64](https://www.msys2.org/)

- [gcc](https://gcc.gnu.org/)
- [clang-cl](https://clang.org/)

Run this in MinGW64:

```console
pacman -Syu
# close and reopen the window
pacman -Su
sudo pacman -S mingw-w64-x86_64-gcc
sudo pacman -S pacman -S mingw-w64-x86_64-clang
```

And you will also need [Visual Studio](https://visualstudio.microsoft.com/downloads/) for release mode

#### Unix

- [gcc](https://gcc.gnu.org/)
- [musl-gcc](https://musl.libc.org/)

Run this in terminal (assuming void linux):

```console
sudo xbps-install -S gcc musl-devel
```

### Executable size

| Platform | Debug (KiB) | Release (KiB) |
| -------- | ----------- | ------------- |
| Windows  | 558         | 263           |
| Unix     | 209         | 93            |

## References

Aka projects used in Mewo

- [Nob](https://github.com/tsoding/nob.h)
- [flag.h (modified)](https://github.com/tsoding/flag.h)

## License

Mewo Build system is licensed under [MIT License](./LICENSE)
