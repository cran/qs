## ----eval=FALSE----------------------------------------------------------
#  library(qs)
#  df1 <- data.frame(x=rnorm(5e6), y=sample(5e6), z=sample(letters,5e6, replace=T))
#  qsave(df1, "myfile.qs")
#  df2 <- qread("myfile.qs")

## ----eval=FALSE----------------------------------------------------------
#  # CRAN version
#  install.packages("qs")
#  
#  # CRAN version compile from source (recommended)
#  remotes::install_cran("qs", type="source", configure.args="--with-simd=AVX2")

## ----eval=FALSE----------------------------------------------------------
#  data.frame(a=rnorm(5e6),
#             b=rpois(100,5e6),
#             c=sample(starnames$IAU,5e6,T),
#             d=sample(state.name,5e6,T),
#             stringsAsFactors = F)

## ----echo=FALSE----------------------------------------------------------
df <- read.csv("df_bench_summary.csv", check.names=F, stringsAsFactors=F)
df$`Write Time (s)` <- signif(df$`Write Time (s)`, 3)
df$`Read Time (s)` <- signif(df$`Read Time (s)`, 3)
df$`File Size (Mb)` <- signif(df$`File Size (Mb)`, 3)
knitr::kable(df)

## ----eval=FALSE----------------------------------------------------------
#  # With byte shuffling
#  x <- 1:1e8
#  qsave(x, "mydat.qs", preset="custom", shuffle_control=15, algorithm="zstd")
#  cat( "Compression Ratio: ", as.numeric(object.size(x)) / file.info("mydat.qs")$size, "\n" )
#  # Compression Ratio:  1389.164
#  
#  # Without byte shuffling
#  x <- 1:1e8
#  qsave(x, "mydat.qs", preset="custom", shuffle_control=0, algorithm="zstd")
#  cat( "Compression Ratio: ", as.numeric(object.size(x)) / file.info("mydat.qs")$size, "\n" )
#  # Compression Ratio:  1.479294

## ----eval=FALSE----------------------------------------------------------
#  df1 <- data.frame(x = randomStrings(1e6), y = randomStrings(1e6), stringsAsFactors = F)
#  qsave(df1, "temp.qs")
#  rm(df1); gc() ## remove df1 and call gc for proper benchmarking
#  
#  # With alt-rep
#  system.time(qread("temp.qs", use_alt_rep=T))[1]
#  #     0.109 seconds
#  
#  
#  # Without alt-rep
#  gc(verbose=F)
#  system.time(qread("temp.qs", use_alt_rep=F))[1]
#  #     1.703 seconds

