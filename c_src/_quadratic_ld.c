/* The batman package: fast computation of exoplanet transit light curves
 * Copyright (C) 2015 Laura Kreidberg	 
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <Python.h>
#include "numpy/arrayobject.h"
#include <math.h>

#if defined (_OPENMP)
#  include <omp.h>
#endif

#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define MAX(x, y) (((x) > (y)) ? (x) : (y))

double ellpic_bulirsch(double n, double k);
double ellec(double k);
double ellk(double k);
static PyObject *_quadratic_ld(PyObject *self, PyObject *args);

static PyObject *_quadratic_ld(PyObject *self, PyObject *args)
{
/*	Input: *************************************
 
	 ds   	array of impact parameters in units of rs
	 c1   	linear    limb-darkening coefficient (gamma_1 in Mandel & Agol 2002)
	 c2   	quadratic limb-darkening coefficient (gamma_2)
	 p    	occulting star size in units of rs
	
	 Output: ***********************************
	
	 flux 	fraction of flux at each ds for a limb-darkened source
	
	 Limb darkening has the form:
	 I(r) = [1 - c1 * (1 - sqrt(1 - (r/rs)^2)) - c2*(1 - sqrt(1 - (r/rs)^2))^2]/(1 - c1/3 - c2/6)/pi
*/
	int nd, nthreads;
	double c1, c2, p, *mu,  *lambdad, *etad, \
		*lambdae, lam, x1, x2, x3, d, omega, kap0 = 0.0, kap1 = 0.0, \
		q, Kk, Ek, Pk, n;
	PyArrayObject *ds, *flux;
	npy_intp i, dims[1];

  	if(!PyArg_ParseTuple(args,"Odddi", &ds, &p, &c1, &c2, &nthreads)) return NULL;

	dims[0] = PyArray_DIMS(ds)[0]; 
	flux = (PyArrayObject *) PyArray_SimpleNew(1, dims, PyArray_TYPE(ds));	//creates numpy array to store return flux values
	nd = (int)dims[0];

	double *f_array = PyArray_DATA(flux);
	double *d_array = PyArray_DATA(ds);

	/*
		NOTE:  the safest way to access numpy arrays is to use the PyArray_GETITEM and PyArray_SETITEM functions.
		Here we use a trick for faster access and more convenient access, where we set a pointer to the 
		beginning of the array with the PyArray_DATA (e.g., f_array) and access elements with e.g., f_array[i].
		Success of this operation depends on the numpy array storing data in blocks equal in size to a C double.
		If you run into trouble along these lines, I recommend changing the array access to something like:
			d = PyFloat_AsDouble(PyArray_GETITEM(ds, PyArray_GetPtr(ds, &i))); 
		where ds is a numpy array object.
		Laura Kreidberg 07/2015
	*/


	lambdad = (double *)malloc(nd*sizeof(double));
	lambdae = (double *)malloc(nd*sizeof(double));
	etad = (double *)malloc(nd*sizeof(double));
	mu = (double *)malloc(nd*sizeof(double));

	if(fabs(p - 0.5) < 1.0e-3) p = 0.5;

	omega = 1.0 - c1/3.0 - c2/6.0;

	#if defined (_OPENMP)
	omp_set_num_threads(nthreads);
	#endif

	#if defined (_OPENMP)
	#pragma omp parallel for private(d, x1, x2, x3, n, q, Kk, Ek, Pk, kap0, kap1)
	#endif
	for(i = 0; i < dims[0]; i++)
	{	
		d = d_array[i]; 		// separation of centers
		x1 = pow((p - d), 2.0);
		x2 = pow((p + d), 2.0);
		x3 = p*p - d*d;

		//source is unocculted:
		if(d >= 1.0 + p)					
		{
			//printf("zone 1\n");
			lambdad[i] = 0.0;
			etad[i] = 0.0;
			lambdae[i] = 0.0;
			f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;

			continue;
		}
		//source is completely occulted:
		if(p >= 1.0 && d <= p - 1.0)			
		{
			//printf("zone 2\n");
			lambdad[i] = 0.0;
			etad[i] = 0.5;		//error in Fortran code corrected here, following Jason Eastman's python code
			lambdae[i] = 1.0;
			f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*(lambdad[i] + 2.0/3.0) + c2*etad[i])/omega;
			continue;
		}
		//source is partly occulted and occulting object crosses the limb:
		if(d >= fabs(1.0 - p) && d <= 1.0 + p)	
		{				
			//printf("zone 3\n");
			kap1 = acos(MIN((1.0 - p*p + d*d)/2.0/d, 1.0));
			kap0 = acos(MIN((p*p + d*d - 1.0)/2.0/p/d, 1.0));
			lambdae[i] = p*p*kap0 + kap1;
			lambdae[i] = (lambdae[i] - 0.50*sqrt(MAX(4.0*d*d - pow((1.0 + d*d - p*p), 2.0), 0.0)))/M_PI;
		}
		//occulting object transits the source but doesn't completely cover it:
		if(d <= 1.0 - p)				
		{					
			//printf("zone 4\n");
			lambdae[i] = p*p;
		}
		//edge of the occulting star lies at the origin
		if(fabs(d - p) < 1.0e-4*(d + p))		
		{
			d = p;
			//printf("zone 5\n");
			if(p == 0.5)	
			{
				//printf("zone 6\n");
				lambdad[i] = 1.0/3.0 - 4.0/M_PI/9.0;
				etad[i] = 3.0/32.0;
				f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;
				continue;
			}
			else if(d >= 0.5)
			{
				//printf("zone 5.1\n");
				lam = 0.5*M_PI;
				q = 0.5/p;
				Kk = ellk(q);
				Ek = ellec(q);
				lambdad[i] = 1.0/3.0 + 16.0*p/9.0/M_PI*(2.0*p*p - 1.0)*Ek -  \
				 	 	(32.0*pow(p, 4.0) - 20.0*p*p + 3.0)/9.0/M_PI/p*Kk;
				etad[i] = 1.0/2.0/M_PI*(kap1 + p*p*(p*p + 2.0*d*d)*kap0 -  \
				              	(1.0 + 5.0*p*p + d*d)/4.0*sqrt((1.0 - x1)*(x2 - 1.0)));
			//	continue;
			}
			else if(d<0.5)	
			{
				//printf("zone 5.2\n");
				lam = 0.50*M_PI;
				q = 2.0*p;
				Kk = ellk(q);
				Ek = ellec(q);
				lambdad[i] = 1.0/3.0 + 2.0/9.0/M_PI*(4.0*(2.0*p*p - 1.0)*Ek + (1.0 - 4.0*p*p)*Kk);
				etad[i] = p*p/2.0*(p*p + 2.0*d*d);
				f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;
				continue;
			}
			f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;
			continue;
		}
		 //occulting star partly occults the source and crosses the limb:
		//if((d > 0.5 + fabs(p  - 0.5) && d < 1.0 + p) || (p > 0.5 && d > fabs(1.0 - p)*1.0001 \
		//&& d < p))  //the factor of 1.0001 is from the Mandel/Agol Fortran routine, but gave bad output for d near fabs(1-p)
		if((d > 0.5 + fabs(p  - 0.5) && d < 1.0 + p) || (p > 0.5 && d > fabs(1.0 - p) \
			&& d < p))
		{
			//printf("zone 3.1\n");
			lam = 0.50*M_PI;
			q = sqrt((1.0 - pow((p - d), 2.0))/4.0/d/p);
			Kk = ellk(q);
			Ek = ellec(q);
			n = 1.0/x1 - 1.0;
			Pk = ellpic_bulirsch(n, q);
			lambdad[i] = 1.0/9.0/M_PI/sqrt(p*d)*(((1.0 - x2)*(2.0*x2 +  \
			        x1 - 3.0) - 3.0*x3*(x2 - 2.0))*Kk + 4.0*p*d*(d*d +  \
			        7.0*p*p - 4.0)*Ek - 3.0*x3/x1*Pk);
			if(d < p) lambdad[i] += 2.0/3.0;
			etad[i] = 1.0/2.0/M_PI*(kap1 + p*p*(p*p + 2.0*d*d)*kap0 -  \
				(1.0 + 5.0*p*p + d*d)/4.0*sqrt((1.0 - x1)*(x2 - 1.0)));
			f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;
			continue;
		}
		//occulting star transits the source:
		if(p <= 1.0  && d <= (1.0 - p)*1.0001)	
		{
			//printf("zone 4.1\n");
			lam = 0.50*M_PI;
			q = sqrt((x2 - x1)/(1.0 - x1));
			Kk = ellk(q);
			Ek = ellec(q);
			n = x2/x1 - 1.0;
			Pk = ellpic_bulirsch(n, q);
			
			lambdad[i] = 2.0/9.0/M_PI/sqrt(1.0 - x1)*((1.0 - 5.0*d*d + p*p +  \
			         x3*x3)*Kk + (1.0 - x1)*(d*d + 7.0*p*p - 4.0)*Ek - 3.0*x3/x1*Pk);
			if(d < p) lambdad[i] += 2.0/3.0;
			if(fabs(p + d - 1.0) <= 1.0e-4)
			{
				lambdad[i] = 2.0/3.0/M_PI*acos(1.0 - 2.0*p) - 4.0/9.0/M_PI* \
				            sqrt(p*(1.0 - p))*(3.0 + 2.0*p - 8.0*p*p);
			}
			etad[i] = p*p/2.0*(p*p + 2.0*d*d);
		}
		f_array[i] = 1.0 - ((1.0 - c1 - 2.0*c2)*lambdae[i] + (c1 + 2.0*c2)*lambdad[i] + c2*etad[i])/omega;
	}
	free(lambdae);
	free(lambdad);
	free(etad);
	free(mu);

	return PyArray_Return((PyArrayObject *)flux);
}
	
/*

   Computes the complete elliptical integral of the third kind using
   the algorithm of Bulirsch (1965):

   Bulirsch 1965, Numerische Mathematik, 7, 78
   Bulirsch 1965, Numerische Mathematik, 7, 353

 INPUTS:

    n,k - int(dtheta/((1-n*sin(theta)^2)*sqrt(1-k^2*sin(theta)^2)),0, pi/2)

 RESULT:

    The complete elliptical integral of the third kind

 -- translated from the ellpic_bulirsch.pro routine from EXOFAST
    (Eastman et al. 2013, PASP 125, 83) by Laura Kreidberg (7/22/15)
*/

double ellpic_bulirsch(double n, double k)
{
	double kc = sqrt(1.-k*k);	
	double p = sqrt(n + 1.);
	double m0 = 1.;
	double c = 1.;
	double d = 1./p;
	double e = kc;
	double f, g;

	int nit = 0;

	while(nit < 10000)
	{
		f = c;
		c = d/p + c;
		g = e/p;
		d = 2.*(f*g + d);
		p = g + p;
		g = m0;
		m0 = kc + m0;
		if(fabs(1.-kc/g) > 1.0e-8)
		{
			kc = 2.*sqrt(e);
			e = kc*m0;
		}
		else
		{
			return 0.5*M_PI*(c*m0+d)/(m0*(m0+p));
		}
		nit++;
	}
	printf("Convergence failure in ellpic_bulirsch\n");
	return 0;
}

double ellec(double k)
{
	double m1, a1, a2, a3, a4, b1, b2, b3, b4, ee1, ee2, ellec;
	// Computes polynomial approximation for the complete elliptic
	// integral of the second kind (Hasting's approximation):
	m1 = 1.0 - k*k;
	a1 = 0.44325141463;
	a2 = 0.06260601220;
	a3 = 0.04757383546;
	a4 = 0.01736506451;
	b1 = 0.24998368310;
	b2 = 0.09200180037;
	b3 = 0.04069697526;
	b4 = 0.00526449639;
	ee1 = 1.0 + m1*(a1 + m1*(a2 + m1*(a3 + m1*a4)));
	ee2 = m1*(b1 + m1*(b2 + m1*(b3 + m1*b4)))*log(1.0/m1);
	ellec = ee1 + ee2;
	return ellec;
}

double ellk(double k)
{
	double a0, a1, a2, a3, a4, b0, b1, b2, b3, b4, ellk,  ek1, ek2, m1;
	// Computes polynomial approximation for the complete elliptic
	// integral of the first kind (Hasting's approximation):
	m1 = 1.0 - k*k;
	a0 = 1.38629436112;
	a1 = 0.09666344259;
	a2 = 0.03590092383;
	a3 = 0.03742563713;
	a4 = 0.01451196212;
	b0 = 0.5;
	b1 = 0.12498593597;
	b2 = 0.06880248576;
	b3 = 0.03328355346;
	b4 = 0.00441787012;
	ek1 = a0 + m1*(a1 + m1*(a2 + m1*(a3 + m1*a4)));
	ek2 = (b0 + m1*(b1 + m1*(b2 + m1*(b3 + m1*b4))))*log(m1);
	ellk = ek1 - ek2;
	return ellk;
}

static char _quadratic_ld_doc[] = "This extension module returns a limb darkened light curve for a quadratic stellar intensity profile.";

static PyMethodDef _quadratic_ld_methods[] = {
  {"_quadratic_ld", _quadratic_ld, METH_VARARGS, _quadratic_ld_doc},{NULL}};

#if PY_MAJOR_VERSION >= 3
	static struct PyModuleDef _quadratic_ld_module = {
		PyModuleDef_HEAD_INIT,
		"_quadratic_ld",
		_quadratic_ld_doc,
		-1, 
		_quadratic_ld_methods
	};

	PyMODINIT_FUNC
	PyInit__quadratic_ld(void)
	{
		PyObject* module = PyModule_Create(&_quadratic_ld_module);
		if(!module)
		{
			return NULL;
		}
		import_array(); 
		return module;
	}
#else

	void init_quadratic_ld(void)
	{
	  	Py_InitModule("_quadratic_ld", _quadratic_ld_methods);
		import_array(); 
	}
#endif

