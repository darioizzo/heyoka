.. _tut_d_output:

Dense output
============

.. versionadded:: 0.4.0

One of the peculiar features of Taylor's method is that it directly provides,
via the Taylor series :eq:`tts_01`, *dense* (or *continuous*) output.
That is, the Taylor series built by the integrator at each timestep can be used
to compute the solution of the ODE system at *any time* within the timestep
(and not only at the endpoint) via polynomial evaluation.

Because the construction of the Taylor series is part of the timestepping algorithm,
support for dense output comes at essentially no extra
cost. Additionally, because the dense output is computed via the
Taylor series of the solution of the ODE system, its accuracy
is guaranteed to respect the error tolerance set in the integrator.

Dense output for the ``step()`` functions
-----------------------------------------

In order to illustrate how to use dense output in heyoka,
we will keep things simple and go back to the simple pendulum:

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 17-28

Enabling dense output in heyoka is a two-step process.

The first step is to invoke one of the ``step()`` functions
passing an extra boolean parameter set to ``true``:

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 30-32

The extra ``true`` function argument instructs the integrator
to record into an internal array the list of Taylor series
coefficients that were generated by the timestepping algorithm.
We can fetch a reference to the list of Taylor coefficients
via the ``get_tc()`` member function:

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 34-37

The Taylor coefficients are stored in row-major order,
each row referring to a different state variable. The number
of columns is the Taylor order of the integrator plus one.
Thus, index 0 in the array refers to the zero-order coefficient
for the ``x`` variable, while index ``ta.get_order() + 1`` refers
to the zero-order coefficient for the ``v`` variable:

.. code-block:: console

   TC of order 0 for x: 0.05
   TC of order 0 for v: 0.025

Indeed, the zero-order Taylor coefficients for the state variables are nothing but
the initial conditions at the beginning of the timestep that was just taken.

.. important::

   This last point is important and needs to be stressed again: the list of Taylor
   coefficients always refers to the **last** step taken and **not** to the next
   step that the integrator might take.

We are now ready to ask the integrator to compute the value of the solution at some
arbitrary time. Let's pick :math:`t = 0.1`,
which is about halfway through the timestep that was just taken:

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 39-42

.. code-block:: console

   x(0.1) = 0.0500303
   y(0.1) = -0.024398

The ``update_d_output()`` member function takes in input an *absolute* time coordinate
and returns a reference to an internal array that will contain the state of the system
at the specified time coordinate, as computed by the evaluation of the Taylor series.
``update_d_output()`` can also be called with a time coordinate *relative* to the current
time by passing ``true`` as a second function argument.

Let's now ask for the dense output at the very end of the timestep that was just taken,
and let's compare it to the current state vector:

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 44-50

.. code-block:: console

   x rel. difference: 0
   v rel. difference: -0

That is, as expected, the dense output at the end of the previous timestep
matches the current state of the system to machine precision.

Before concluding, we need to highlight a couple of caveats regarding the
use of dense output.

.. important::

   First, it is the user's responsibility to ensure that the array of Taylor
   coefficients contains up-to-date values. In other words, the user needs to remember
   to invoke the ``step()`` functions with the extra boolean argument set to ``true``
   before invoking ``update_d_output()``.
   Failure to do so will result in ``update_d_output()`` producing incorrect values.

.. important::

   Second, the accuracy of dense output is guaranteed to match the integrator's
   accuracy only if the time coordinate falls within the last step taken. Note that heyoka will
   **not** prevent the invocation of ``update_d_output()`` with time coordinates outside the
   guaranteed accuracy range - it is the user's responsibility to be aware
   that doing so will produce results whose accuracy does not match the integrator's
   error tolerance.

Dense output for the ``propagate_*()`` functions
------------------------------------------------

.. versionadded:: 0.8.0

Dense output can be enabled also for the time-limited propagation functions
``propagate_for()`` and ``propagate_until()`` via the boolean keyword argument
``write_tc``.

When ``write_tc`` is set to ``true``, the ``propagate_*()`` functions
will internally invoke the ``step()`` function with the optional boolean
flag set to ``true``, so that at the end of each timestep the Taylor coefficients
will be available. The Taylor coefficients can be used, e.g., inside the
optional callback that can be passed to the ``propagate_*()`` functions.

Note that ``propagate_grid()`` always unconditionally writes the Taylor coefficients
at the end of each timestep.

Full code listing
-----------------

.. literalinclude:: ../tutorial/d_output.cpp
   :language: c++
   :lines: 9-
