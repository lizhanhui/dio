load("@rules_cc//cc:defs.bzl", "cc_binary", "cc_import")


cc_binary(
    name = "dio",
    srcs = ["main.cc"],
    deps = [
        "@gflags//:gflags",
    ],
    linkopts = [
        "-luring",
    ],
)