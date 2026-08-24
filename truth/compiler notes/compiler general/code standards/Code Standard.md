Giant switch stattements are a code smell, especially inside of main functions.
Factor massive control logic blocks into a dictionary ADT instead

Do not overengineer one off functions with huge design patterns


Anything that is present in all classes of a similar type should be factored out into a parent class, e.g., ASTNode containing visit logic

Functions containing multiple large chunks of logic, a function shuold do one thing, if a function has mutliple large steps, factor steps into helper functions and have that function call the helper functions.

Complex large chunks of logic should have concise coments explaining what the logic does.


Avoid large lambda functions, those should be named helper functions instead. Lambdas are for small concise function definitions that are inlined with stl algortihms etc.

