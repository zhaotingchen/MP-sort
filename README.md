MP-sort
=======

A Massively Parallel Sorting Library

Install
-------

Use the Makefile to build / install the .a targets, and link against 

.. code :: bash

    -lradixsort -lradixsort-mpi

Makefile supports overrideing :code:`CC`, :code:`MPICC` and :code:`CFLAGS`

The python binding can be installed with

.. code :: bash

    python setup.py install [--user]

The same Makefile overrides are supported, if the environment variables are set.

Usage: C
--------

The basic C interface is:

.. code :: c

    void radix_sort_mpi(void * base, size_t nmemb, size_t size,
        void (*radix)(const void * ptr, void * radix, void * arg), 
        size_t rsize, 
        void * arg, MPI_Comm comm);

    Parameters
    ----------
    base :
        base pointer of the local data
    nmemb :
        number of items in the local data
    elsize :
        size of an item in the local data
    radix (ptr, radix, arg):
        the function calculates the radix of an item at ptr;
        the raxis shall be stored at memory location pointed to radix
    rsize :
        the size of radix array in bytes; only the behavior for 8 is well-defined:
        the radix is interpreted as uint64.
    arg   :
        argument to pass into radix()
    comm  :
        the MPI communicator for the sort. 

Usage: Python
-------------

The basic Python interface is:

.. code :: c
    
    import mpsort

    mpsort.sort(localdata, orderby=None)

    Sort an distributed array in place.

    Parameters
    ----------
    localdata : array_like
        local data, must be C_CONTIGUOUS, and of a struct-dtype.
        for example, :code:`localdata = numpy.empty(10, dtype=[('key', 'i4'), ('value', 'f4')])`.
    orderby : scalar
        the field to be sorted by. The field must be of an integral type. 'i4', 'i8', 'u4', 'u8'.
    
        
    


    


