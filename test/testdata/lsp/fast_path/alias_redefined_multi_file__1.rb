# typed: true
# TODO explain this spacer line
# TODO explain this spacer line

class A
  def to; end
end

A.new.to
A.new.to1 # error: Method `to1` does not exist on `A`
A.new.from
A.new.from1 # error: Method `from1` does not exist on `A`
