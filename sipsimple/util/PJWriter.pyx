# cython: language_level=2

import sys

cdef extern from "writer.c" nogil:
    int open_pipe_port(char *path)
    int close_pipe_port(int fd)
    int write_pipe_port(int fd, void *samples, unsigned int count)

cdef class PJWriter:
    cdef int fd

    def __init__(self):
        self.fd = -1;

    cpdef int Open(self, char *path):
        with nogil:
            self.fd = open_pipe_port(path)
            return self.fd

    cpdef int Close(self):
        with nogil:
            return close_pipe_port(self.fd)

    cpdef int Write(self, char *data, int len):
        with nogil:
            return write_pipe_port(self.fd, data, len)
