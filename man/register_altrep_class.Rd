% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/zz_help_files.R
\name{register_altrep_class}
\alias{register_altrep_class}
\title{Register ALTREP class for serialization}
\usage{
register_altrep_class(classname, pkgname)
}
\arguments{
\item{classname}{The ALTREP class name}

\item{pkgname}{The package the ALTREP class comes from}
}
\description{
Register an ALTREP class to serialize using base R serialization.
}
\examples{
register_altrep_class("compact_intseq", "base")
}
