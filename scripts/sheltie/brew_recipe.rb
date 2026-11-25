# frozen_string_literal: true

# Run with `brew ruby brew_recipe.rb info|fetch RECIPE`. Homebrew 7 removed loading formulae
# from paths on the command line; the library still loads an installed keg's own recipe.
require "formula"

command, recipe = ARGV
formula = Formulary.from_path(Pathname(recipe))
case command
when "info"
  puts JSON.generate({ "formulae" => [formula.to_hash_with_variations] })
when "fetch"
  spec = formula.stable
  spec.fetch
  spec.resources.each_value(&:fetch)
  spec.patches.select(&:external?).each { |patch| patch.resource.fetch }
else
  abort "usage: brew ruby brew_recipe.rb info|fetch RECIPE"
end
