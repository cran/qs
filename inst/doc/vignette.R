## ------------------------------------------------------------------------
library(qs)
x1 <- data.frame(int = sample(1e3, replace=T), num = rnorm(1e3), char = qs::randomStrings(1e3), stringsAsFactors = F)
qsave(x1, "mydata.qs")

x2 <- qread("mydata.qs")
identical(x1, x2) # returns true

