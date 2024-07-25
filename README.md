# Remote fetch

This is a PoC to demonstrate how to use the `fanotify` pre-access hooks.  There
are 3 tools here

- `populate`
- `remote-fetch`
- `mmap-validate`

## `populate`

This is designed to mirror a source directory in a destination directory.  It
will create all the files and directories, and truncate all the files to the
size in the source directory, but will leave them otherwise empty.  This must be
done before using `remote-fetch`

## `remote-fetch`

This is the tool that handles the pre-access hooks.  You point it at the source
directory and the destination directory that you populated with `populate`.
Then you can access any files you want in the destination directory and
`remote-fetch` will on-demand populate the ranges that are accessed in the
files.

## `mmap-validate`

This is to test the `mmap()` access to the regions.  You use this tool to create
the file and then to validate.  An example test would look like this

```
# mkdir src dst
# ./mmap-validate create src/file
# ./populate src dst
# ./remote-fetch src dst
# ./mmap-validate validate dst/file
```

This should exit with no errors to indicate everything is working.
