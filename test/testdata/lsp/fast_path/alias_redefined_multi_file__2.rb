# typed: true
# TODO explain this spacer line

class A
  alias_method :from, :to
end

A.new.to
A.new.to1 # error: Method `to1` does not exist on `A`
A.new.from
A.new.from1 # error: Method `from1` does not exist on `A`
