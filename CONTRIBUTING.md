# Contributing

Built something that shows Fugleramme on a new device? Share it here!

## Adding your program

1. Put it in a folder named after the hardware and the client's language, like `ee04-micropython`.
2. Give the folder a README that explains:
   - what hardware it runs on
   - how to install and configure it
   - how to update it
   - what you tested it on, and how
3. Add a row to the table in the [README](README.md), and your folder and GitHub username to [`.github/CODEOWNERS`](.github/CODEOWNERS).
4. Open a pull request.

## Owning a program

Once your program is merged, you're its owner. You're asked to review pull requests that changes your folder, of course with help from the repo admins if needed.

Can't keep your program up to date anymore? Just say so - it can stay in the repo, marked as unmaintained or inherited by someone else.

## Commits

We use [conventional commits](https://www.conventionalcommits.org/), with your folder as scope:

```
feat(ee02-arduino): add a low battery notice
fix(ee04-micropython): retry when Fugleramme does not respond
```

Pull requests are squashed, so the title becomes the commit message.

## License

By contributing, you release your code under the repo's [MIT license](LICENSE).
