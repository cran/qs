---
title: "README"
author: Travers Ching
---

  # qs [![Build Status](https://travis-ci.org/traversc/qs.svg)](https://travis-ci.org/traversc/qs)

Quick serialization of R objects

This package provides an interface for quickly writing (serializing) and reading (de-serializing) objects to and from disk.  The goal of this package is to provide a lightning-fast and complete replacement for the `saveRDS` and `readRDS` functions in R.  

Inspired by the `fst` package, `qs` uses a similar block-compression approach using the `zstd` library and direct "in memory" compression, which allows for lightning quick serialization.  It differs in that it uses a more general approach for attributes and object references for common data types (numeric data, strings, lists, etc.), meaning any S3 object built on common data types, e.g., `tibble`s, time-stamps, `bit64`, etc. can be serialized.  For less common data types (formulas, environments, functions, etc.), `qs` relies on built in R serialization functions via the `RApiSerialize` package followed by block-compression.  

For character vectors, `qs` also uses the alt-rep system to quickly read in string data.  

## Installation
`devtools::install_git("traversc/qs")`

(Requires R version 3.5 or higher)

## Features
The table below compares the features of different serialization approaches in R.


|                    | qs         | fst           | saveRDS  |
|--------------------|:-----------:|:---------------:|:----------:|
| Not Slow             | &#10004;   | &#10004;       | X |
| Numeric Vectors    | &#10004;   | &#10004;       | &#10004;  |
| Integer Vectors    | &#10004;   | &#10004;       | &#10004;  |
| Logical Vectors    | &#10004;   | &#10004;       | &#10004;  |
| Character Vectors  | &#10004;   | &#10004;       | &#10004;  |
| Character Encoding | &#10004;   | (vector-wide only) | &#10004;  |
| Complex Vectors    | &#10004;   | X      | &#10004;  |
| Data.Frames        | &#10004;   | &#10004;       | &#10004;  |
| On disk row access | X  | &#10004;       | X |
| Attributes         | &#10004;   | Some          | &#10004;  |
| Lists / Nested Lists| &#10004;   |  X     | &#10004;  |
| Multi-threaded     | X (Not Yet) | &#10004;      |  X   |

## Summary Benchmarks
The table below lists serialization speed for several different data types.  
<table>
  <tr>
    <th></th>
    <th colspan="2">qs</th>
    <th colspan="2">saveRDS</th>
    <th colspan="2">fst<br>1 thread</th>
    <th colspan="2">fst<br>4 threads</th>
  </tr>
  <tr>
    <td></td>
    <td>Write</td>
    <td>Read</td>
    <td>Write</td>
    <td>Read</td>
    <td>Write</td>
    <td>Read</td>
    <td>Write</td>
    <td>Read</td>
  </tr>
  <tr>
    <td><b>Integer Vector</b><br>sample(1e8)</td>
    <td>1015.2 MB/s<br></td>
    <td>889.8 MB/s</td>
    <td>27.1 MB/s</td>
    <td>135.5 MB/s</td>
    <td>686.6 MB/s</td>
    <td>442.4 MB/s</td>
    <td>699.1 MB/s</td>
    <td>567.9 MB/s</td>
  </tr>
  <tr>
    <td><b>Numeric Vector</b><br>runif(1e8)</td>
    <td>861.2 MB/s</td>
    <td>954.0 MB/s</td>
    <td>24.3 MB/s</td>
    <td>131.9 MB/s</td>
    <td>744.0 MB/s</td>
    <td>638.7 MB/s</td>
    <td>754.4 MB/s</td>
    <td>848.0 MB/s</td>
  </tr>
  <tr>
    <td><b>Character Vector</b><br>qs::randomStrings(1e7)</td>
    <td>1312.9 MB/s</td>
    <td>715.8 MB/s*</td>
    <td>49.1 MB/s</td>
    <td>43.9 MB/s</td>
    <td>1440.9 MB/s</td>
    <td>59.5 MB/s</td>
    <td>1536.3 MB/s</td>
    <td>59.3 MB/s</td>
  </tr>
  <tr>
    <td><b>List</b><br>map(1:1e5,sample(100))</td>
    <td>197.2 MB/s<br></td>
    <td>311.5 MB/s</td>
    <td>7.7 MB/s</td>
    <td>123.5 MB/s</td>
    <td>N/A</td>
    <td>N/A</td>
    <td>N/A</td>
    <td>N/A</td>
  </tr>
  <tr>
    <td><b>Environment</b><br>map(1:1e5,sample(100))<br>names(x)&lt;-1:1e5<br>as.environment(x)</td>
    <td>56.0 MB/s</td>
    <td>117.5 MB/s</td>
    <td>7.7 MB/s</td>
    <td>89.6 MB/s</td>
    <td>N/A</td>
    <td>N/A</td>
    <td>N/A</td>
    <td>N/A</td>
  </tr>
</table>

## Additional Benchmarks

### Data.Frame benchmark

Benchmarks for serializing and de-serializing large data.frames (5 million rows) composed of a numeric column (`rnorm`), an integer column (`sample(5e6)`), and a character vector column (random alphanumeric strings of length 50).  See `dataframe_bench.png` for a comparison using different compression parameters.  

This benchmark also includes materialization of alt-rep data, for an apples-to-apples comparison.  

#### Serialization speed with default parameters:
| Method         | write time (s) | read time (s) |
|----------------|----------------|---------------|
| qs             | 0.49391294     | 8.8818166     |
| fst (1 thread) | 0.37411811     | 8.9309314     |
| fst (4 thread) | 0.3676273      | 8.8565951     |
| saveRDS        | 14.377122      | 12.467517     |

#### Serialization speed with different parameters

The numbers in the figure reflect the compression parameter used.  `qs` uses the `zstd` compression library, and compression parameters range from -50 to 22 (`qs` uses a default value of -1).  `fst` defines it's own compression range through a combination of `zstd` and `lz4` algorithms, ranging from 0 to 100 (default: 0).  

<img src="https://raw.githubusercontent.com/traversc/qs/master/vignettes/dataframe_bench.png" width="576">

### Nested List benchmark
Benchmarks for serialization of random nested lists with random attributes (approximately 50 Mb).  See the nested list example in the tests folder.  

#### Serialization speed with default parameters:
| Method  | write time (s) | read time (s) |
|---------|----------------|---------------|
| qs      | 0.17840716     | 0.19489372    |
| saveRDS | 3.484225       | 0.58762548    |

<img src="https://raw.githubusercontent.com/traversc/qs/master/vignettes/nested_list_bench.png" width="576">
