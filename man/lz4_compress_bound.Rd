% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/zz_help_files.R
\name{lz4_compress_bound}
\alias{lz4_compress_bound}
\title{lz4 compress bound}
\usage{
lz4_compress_bound(size)
}
\arguments{
\item{size}{An integer size.}
}
\value{
Maximum compressed size.
}
\description{
Exports the compress bound function from the lz4 library. Returns the maximum compressed size of an object of length \code{size}.
}
\examples{
lz4_compress_bound(100000)
#' lz4_compress_bound(1e9)
}
