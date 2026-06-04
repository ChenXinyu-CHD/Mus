# My C-like language

> [!WARNING]
> The project is designed for my personal needs only, and may not be suitable for everybody. I do not offer any support for it either. It's completely Open Source, fork it and adapt it to your needs.

> [!WARNING]
> This language is work in progress

## How to use

First build:

```console
$ cc -o nob nob.c
```

After that, you can use nob to build the compiler.

```console
$ ./nob
```

This will automatic build nob itself, and build the compiler named mcc,
which can be used as follow:

```console
$ ./mcc -o hello example/hello_world.mus && ./hello
```

or you can use the following for convenience.

```console
$ ./mcc -r example/hello_world.mus
```

## 3rd lib used

- https://github.com/tsoding/nob.h
- https://github.com/tsoding/ht.h

### The following is no longer used, thanks for their contributions to the community

- https://github.com/nothings/stb/blob/master/stb_c_lexer.h
