#include <stdio.h>
#include <stdlib.h>
#include <vicNl.h>

static char vcid[] = "$Id$";

double solve_snow(char                 overstory,
		  double               BareAlbedo,
		  double               LongUnderOut, // LW from understory
		  double               MIN_RAIN_TEMP,
		  double               MAX_SNOW_TEMP,
		  double               Tcanopy, // canopy air temperature
		  double               Tgrnd, // soil surface temperature
		  double               air_temp, // air temperature
		  double               density,
		  double               dp,
		  double               ice0,
		  double               longwave,
		  double               moist,
		  double               mu,
		  double               prec,
		  double               pressure,
		  double               shortwave,
		  double               snow_grnd_flux,
		  double               vp,
		  double               vpd,
		  double               wind_h,
		  double              *AlbedoUnder,
		  double              *Evap,
		  double              *Le,
		  double              *LongUnderIn, // surface incomgin LW
		  double              *NetLongSnow, // net LW at snow surface
		  double              *NetShortGrnd, // net SW reaching ground
		  double              *NetShortSnow, // net SW at snow surface
		  double              *ShortUnderIn, // surfave incoming SW
		  double              *Torg_snow,
		  double              *aero_resist,
		  double              *aero_resist_used,
		  double              *coverage, // best guess snow coverage
		  double              *delta_coverage, // cover fract change
		  double              *delta_snow_heat, // change in pack heat
		  double              *displacement,
		  double              *gauge_correction,
		  double              *melt_energy,
		  double              *out_prec,
		  double              *ppt,
		  double              *rainfall,
		  double              *ref_height,
		  double              *roughness,
		  double              *snow_inflow,
		  double              *snowfall,
		  double              *surf_atten,
		  double              *wind,
		  float               *root,
		  int                  INCLUDE_SNOW,
		  int                  Nnodes,
		  int                  Nveg,
		  int                  band,
		  int                  hour,
		  int                  iveg,
		  int                  day_in_year,
		  int                  dt,
		  int                  month,
		  int                  day, 
		  int                  year, 
		  int                  rec,
		  int                  veg_class,
		  int                 *UnderStory,
		  energy_bal_struct   *energy,
		  layer_data_struct   *layer_dry,
		  layer_data_struct   *layer_wet,
		  snow_data_struct    *snow,
		  soil_con_struct     *soil_con,
		  veg_var_struct      *veg_var_dry,
		  veg_var_struct      *veg_var_wet) {
/*********************************************************************
  solve_snow.c                Keith Cherkauer       July 2, 1998

  This routine was written to handle the various calls and data 
  handling needed to solve the various components of the new VIC
  snow code for both the full_energy and water_balance models.

  Returns snow, veg_var, and energy variables for each elevation
  band.  Variable ppt[] is defined for elevation bands with snow.

  07-13-98 modified to use elevation bands when solving the 
           snow model                                        KAC
  11-30-98 reworked the way the snow/rain fraction is computed
           and added to check to assure that very small amounts 
           of snow do not fall, causing snow sublimation to 
           be calculated.  (found by Greg)                   KAC
  02-29-00 removed ground heat flux computations, will now 
           make those outside of this routine, in the same function
	   that is used to compute the ground heat flux when
	   there is no snow cover.                           KAC
  10-06-00 added partial snow cover and advection of sensible
           heat from local bare patches.                     KAC
  03-06-01 Modified to pass the minimum depth of full snow cover
           as a variable in soil_con rather than a globally defined
           constant.                                         KAC
  06-15-02 Fixed check of new snow accumulation for setting 
           understory flag to use snowfall[WET] not snowfall.  KAC
  06-15-02 Set MELTING flag to maintain melting albedo curve
           even during brief periods of refreezing, until a 
           snowfall exceeds SnowThres.           .           KAC
  11-18-02 Modified to handle the effects of blowing snow.    LCB
  xx-xx-01 Modified to handle closed canopy energy balance.   KAC
  06-04-03 Added check so that MELTING flag is only TRUE if melt
           occurs in the melt season - currently this is defined
           between March 1 and October 1.  Otherwise the MELTING
           flag can trigger rapid very early season melt	KAC
  05-Aug-04 Removed lag_one, sigma_slope, and fetch from argument
	    list since they were only used in call to snow_melt(),
	    which no longer needs them.				TJB
  28-Sep-04 Added aero_resist_used to store the aerodynamic resistance
	    used in flux calculations.				TJB

*********************************************************************/

  extern option_struct   options;
  extern veg_lib_struct *veg_lib;

  char                ErrStr[MAXSTRING];
  char                FIRST_SOLN[1];
  float               tempstep;
  double              TmpAlbedoUnder[2];
/*   double              LongUnderOut; */
  double              ShortOverIn;
/*   double              Tgrnd; */
  double              Tmp;
  double              melt;
  double              old_coverage;
  double              old_depth;
  double              old_swq;
  double              rainonly;
  double              tmp_Wdew[2];
  double              tmp_grnd_flux;
  double              store_snowfall;
  int                 curr_snow;

  /* initialize moisture variable s*/
  melt     = 0.;
  ppt[WET] = 0.; 
  ppt[DRY] = 0.; 

  /* initialize storage for energy consumed in changing snowpack
     cover fraction */
  (*melt_energy)     = 0.;

  /* initialize change in snow[pack heat storage */
  (*delta_snow_heat) = 0.;

  /** Calculate Fraction of Precipitation that falls as Rain **/
  rainonly      = calc_rainonly(air_temp, prec, MAX_SNOW_TEMP, 
				MIN_RAIN_TEMP, mu); 
  snowfall[WET] = gauge_correction[SNOW] * (prec - rainonly);
  rainfall[WET] = gauge_correction[RAIN] * rainonly;
  snowfall[DRY] = 0.;
  rainfall[DRY] = 0.;
  if ( snowfall[WET] < 1e-5 ) snowfall[WET] = 0.;
  (*out_prec) = snowfall[WET] + rainfall[WET];
  store_snowfall = snowfall[WET];

  /** Compute latent heats **/
  (*Le) = (2.501e6 - 0.002361e6 * air_temp);

  /** verify that distributed precipitation fraction equals 1 if
      snow is present or falling **/
  if ( ( snow->swq > 0 || snowfall[WET] > 0.
	 || (snow->snow_canopy>0. && overstory) ) ) {
    if ( mu != 1 && options.FULL_ENERGY ) {
      sprintf(ErrStr,"Snow model cannot be used if mu (%f) is not equal to 1.\n\tsolve_snow.c: record = %i,\t vegetation type = %i",
	      mu, rec, iveg);
      vicerror(ErrStr);
    }
    else if ( mu != 1 ) {
      fprintf(stderr,"WARNING: Snow is falling, but mu not equal to 1 (%f)\n",
	      mu);
      fprintf(stderr,"\trec = %i, veg = %i, hour = %i\n",rec,iveg,hour);
    }
  }

  /** If first iteration, set UnderStory index **/
  if ( *UnderStory == 999 ) {
    if ( snow->swq > 0 || snowfall[WET] > 0 ) *UnderStory = 2; // snow covered
    else *UnderStory = 0; // understory bare
  }

  /* initialize understory radiation inputs */
  (*ShortUnderIn) = shortwave;
  (*LongUnderIn)  = longwave;

  if ( (snow->swq > 0 || snowfall[WET] > 0.
      || (snow->snow_canopy > 0. && overstory)) && mu==1 ) {
    
    /*****************************
      Snow is Present or Falling 
    *****************************/

    snow->snow = TRUE; // snow is present during time step
	
    if ( !overstory ) (*surf_atten) = 1.;  // understory covered by snow

    old_coverage = snow->coverage; // store previous coverage fraction
      
    /** compute understory albedo **/
    TmpAlbedoUnder[0]   = NEW_SNOW_ALB; // albedo if new snow falls
    if ( snow->swq > 0 ) 
      // age snow albedo if no new snow
      snow->albedo = snow_albedo( snowfall[WET], snow->swq, 
				  snow->coldcontent, dt, 
				  snow->last_snow, snow->MELTING); 
    TmpAlbedoUnder[1]   = (*coverage * snow->albedo
			   + (1. - *coverage) * BareAlbedo); 

    /** Compute Radiation Balance over Snow **/ 
    
    if ( iveg != Nveg ) {
      
      /****************************************
	Check Vegetation for Intercepted Snow
      ****************************************/
      
      if ( overstory ) {

	/***********************************************
          Compute canopy interception of precipitation
        ***********************************************/

	(*ShortUnderIn) *= (*surf_atten);  // SW transmitted through canopy
	ShortOverIn      = (1. - (*surf_atten)) * shortwave; // canopy incident SW
	snow_intercept(density, (double)dt * SECPHOUR, vp, 1., 
		       veg_lib[veg_class].LAI[month-1], 
		       (*Le), longwave, LongUnderOut, 
		       veg_lib[veg_class].Wdmax[month-1], pressure, 
		       ShortOverIn, *ShortUnderIn, 
		       Tcanopy, vpd, 
		       BareAlbedo, mu, &energy->canopy_advection, 
		       &energy->AlbedoOver, TmpAlbedoUnder, 
		       &veg_var_wet->Wdew, &snow->snow_canopy, 
		       &energy->canopy_latent, 
		       &energy->canopy_latent_sub, LongUnderIn, 
		       &energy->canopy_refreeze, &energy->NetLongOver, 
		       &energy->NetShortOver, 
		       aero_resist, aero_resist_used, rainfall, 
		       &energy->canopy_sensible, 
		       snowfall, &energy->Tfoliage, &snow->tmp_int_storage, 
		       &snow->canopy_vapor_flux, wind, displacement, 
		       ref_height, roughness, root, *UnderStory, band, 
		       hour, iveg, month, rec, veg_class, layer_dry, 
		       layer_wet, soil_con, veg_var_dry, veg_var_wet);

	/* Store throughfall from canopy */
	veg_var_wet->throughfall = rainfall[0] + snowfall[0];

	/* Determine under canopy net shortwave */
	if ( veg_var_wet->throughfall > 0 ) {
	  (*AlbedoUnder) = TmpAlbedoUnder[0];
	  *NetShortSnow = ( 1. - *AlbedoUnder ) * *ShortUnderIn; 
	}
	else {
	  (*AlbedoUnder) = TmpAlbedoUnder[1];
	  *NetShortSnow = ( 1. - *AlbedoUnder ) * *ShortUnderIn; 
	}
  

	energy->LongOverIn = longwave;

      }  /* if overstory */

      else if(snowfall[0] > 0. && veg_var_wet->Wdew > 0.) {

	/** If No Overstory, Empty Vegetation of Stored Water **/

	rainfall[WET]            += veg_var_wet->Wdew;
	veg_var_wet->throughfall  = rainfall[WET] + snowfall[WET];
	veg_var_wet->Wdew         = 0.;
	energy->NetLongOver       = 0;
	energy->LongOverIn        = 0;
	energy->Tfoliage             = air_temp;
	(*AlbedoUnder)            = TmpAlbedoUnder[0];
	(*NetShortSnow)          = (1.0 - *AlbedoUnder) * shortwave; 

      } /* snow falling on vegetation with dew */

      else {

	/** Precipitation "Passes Through" Vegetation which 
	    is Under Snow (used only for accounting purposes)**/

	veg_var_wet->throughfall = rainfall[WET] + snowfall[WET];
	veg_var_dry->throughfall = rainfall[DRY] + snowfall[DRY];
	energy->NetLongOver      = 0;
	energy->LongOverIn       = 0;
	energy->Tfoliage    = air_temp;
	if ( snowfall[WET] > 0 ) { // net SW at snow/ground surface
	  (*AlbedoUnder)   = TmpAlbedoUnder[0];
	  (*NetShortSnow) = (1.0 - *AlbedoUnder) * shortwave; 
	}
	else {
	  (*AlbedoUnder)   = TmpAlbedoUnder[1];
	  (*NetShortSnow) = (1.0 - *AlbedoUnder) * shortwave; 
	}

      } /* vegetation already covered by snow */

    }
    else { /* no vegetation present */
      energy->NetLongOver = 0;
      energy->LongOverIn  = 0;
      if ( snowfall[WET] > 0 ) { // net SW at snow/ground surface
	(*AlbedoUnder)   = TmpAlbedoUnder[0];
	(*NetShortSnow) = (1.0 - *AlbedoUnder) * shortwave; 
      }
      else {
	(*AlbedoUnder)   = TmpAlbedoUnder[1];
	(*NetShortSnow) = (1.0 - *AlbedoUnder) * shortwave; 
      }
    }
    
    if ( snow->swq > 0.0 || snowfall[0] > 0 ) {
      
      /******************************
	Snow Pack Present on Ground
      ******************************/

      // store snowfall reaching the ground for determining the albedo
      store_snowfall            = snowfall[WET];

      /** Age Snowpack **/
      if( snowfall[WET] > 0 ) curr_snow = 1; // new snow - reset pack age
      else curr_snow = snow->last_snow + 1; // age pack by one time step
      
      (*NetShortGrnd) = 0.;
   
      (*snow_inflow) += rainfall[WET] + snowfall[WET];

      /** Call snow pack accumulation and ablation algorithm **/

      old_swq       = snow->swq; /* store swq for density calculations */
      (*UnderStory) = 2;         /* ground snow is present of accumulating 
				    during time step */
      
#if SPATIAL_SNOW
      /* make snowpack uniform at mean depth */
      if ( snowfall[WET] > 0 ) snow->coverage = 1;
      if (snow->coverage > 0 && snowfall[WET] == 0) {
	if ( snow->coverage < 1) {
	  /* rain falls evenly over grid cell */
	  ppt[WET] = rainfall[WET] * (1.0 - snow->coverage);
	  rainfall[WET] *= snow->coverage;
	}
      }
#endif

      snow_melt((*Le), (*NetShortSnow), Tcanopy, Tgrnd, 
		roughness, aero_resist[*UnderStory], aero_resist_used,
		air_temp, *coverage, (double)dt * SECPHOUR, density, 
		displacement[*UnderStory], snow_grnd_flux, 
		*LongUnderIn, pressure, rainfall[WET], snowfall[WET], 
		vp, vpd, wind[*UnderStory], ref_height[*UnderStory], 
		NetLongSnow, Torg_snow, &melt, &energy->error, 
		&energy->advected_sensible, &energy->advection, 
		&energy->deltaCC, &tmp_grnd_flux, &energy->latent, 
		&energy->latent_sub, &energy->refreeze_energy, 
		&energy->sensible, INCLUDE_SNOW, band, 
		rec, snow, soil_con);

      // store melt water
      ppt[WET] += melt;

      // store snow albedo
      energy->AlbedoUnder   = TmpAlbedoUnder[1];
      
      /** Compute Snow Parameters **/
      if(snow->swq > 0.) {

	/** Calculate Snow Density **/
	if ( snow->surf_temp <= 0 )
	  // snowpack present, compress and age density
	  snow->density = snow_density(day_in_year, 
				       snowfall[WET], 
				       air_temp, old_swq, snow->depth, 
				       snow->coldcontent, 
				       (double)dt, snow->surf_temp);
	else 
	  // no snowpack present, start with new snow density
	  if ( curr_snow == 1 ) 
	    snow->density = new_snow_density(air_temp);
	
	/** Calculate Snow Depth (H.B.H. 7.2.1) **/
	old_depth   = snow->depth;
	snow->depth = 1000. * snow->swq / snow->density; 

	/** Record if snowpack is melting this time step **/
	if ( snow->coldcontent >= 0 && day_in_year > 60 // ~ March 1
	     && day_in_year < 273 // ~ October 1
	     ) snow->MELTING = TRUE;
	else if ( snow->MELTING && snowfall[WET] > TraceSnow ) 
	  snow->MELTING = FALSE;

	
	/** Check for Thin Snowpack which only Partially Covers Grid Cell
	 exists only if not snowing and snowpack has started to melt **/
#if SPATIAL_SNOW
	snow->coverage = calc_snow_coverage(&snow->store_snow, 
					    soil_con->depth_full_snow_cover, 
					    old_coverage, snow->swq,
					    old_swq, snow->depth, old_depth, 
					    melt + snow->vapor_flux, 
					    &snow->max_swq, snowfall, 
					    &snow->store_swq, 
					    &snow->swq_slope,
					    &snow->store_coverage);

#else

	if ( snow->swq > 0 ) snow->coverage = 1.;
	else snow->coverage = 0.;
#endif

      }
      else {
	snow->coverage = 0.;
      }
      
      *delta_coverage = old_coverage - snow->coverage;

      if ( *delta_coverage != 0 ) {
	
	/* returns mixed surface albedo if snow cover fraction has 
	   decreased (old_coverage is cover fraction for previous
	   time step, snow->coverage is cover fraction for current
	   time step. */
	if ( old_coverage > snow->coverage ) {
	  /* melt has occured */
	  *coverage = (old_coverage);
	  (*AlbedoUnder) = (*coverage - snow->coverage) 
	    / (1. - snow->coverage) * snow->albedo;
	  (*AlbedoUnder) += (1. - *coverage) 
	    / (1. - snow->coverage) * BareAlbedo;

	  /* compute snowpack energy used in reducing coverage area */
	  (*melt_energy) = ( *delta_coverage ) 
	    * (energy->advection - energy->deltaCC 
	       + energy->latent + energy->latent_sub 
	       + energy->sensible + energy->refreeze_energy 
	       + energy->advected_sensible);
	}
	else if ( old_coverage < snow->coverage ) {
#if VERBOSE
	  if ( snow->coverage != 1. ) 
	    fprintf(stderr, "WARNING: snow cover fraction has increased, but it is not equal to 1 (%f).\n", snow->coverage);
#endif // VERBOSE
	  *coverage       = snow->coverage;
	  *delta_coverage = 0;
	}
	else {
	  *coverage       = snow->coverage;
	  *delta_coverage = 0.;
	}
      }
      else if ( old_coverage == 0 && snow->coverage == 0 ) {
	// snow falls and melts all in one time step
	*delta_coverage = 1.;
	*coverage       = 0.;
	(*melt_energy) = (energy->advection - energy->deltaCC 
			  + energy->latent + energy->latent_sub 
			  + energy->sensible + energy->refreeze_energy
			  + energy->advected_sensible);
      }

      /** Compute energy balance components for snowpack */
      
      (*NetLongSnow)     *= (snow->coverage);
      (*NetShortSnow)    *= (snow->coverage);
      (*NetShortGrnd)    *= (snow->coverage);
      energy->latent     *= (snow->coverage + *delta_coverage);
      energy->latent_sub *= (snow->coverage + *delta_coverage);
      energy->sensible   *= (snow->coverage + *delta_coverage);

      if ( snow->swq == 0 ) {

	/** Reset Snow Pack Variables after Complete Melt **/

	/*** NOTE *coverage should not be zero the time step the 
	     snowpack melts - FIX THIS ***/

	snow->density    = 0.;
	snow->depth      = 0.;
	snow->surf_water = 0;
	snow->pack_water = 0;
	snow->surf_temp  = 0;
	snow->pack_temp  = 0;
	snow->coverage   = 0;
	snow->swq_slope  = 0;
	snow->store_snow = TRUE;
	snow->MELTING    = FALSE;
	
      }

      snowfall[WET] = 0; /* all falling snow has been added to the pack */
      rainfall[WET] = 0; /* all rain has been added to the pack */
      
    }

    else {

      /** Ground Snow not Present, and Falling Snow Does not Reach Ground **/

      ppt[WET] += rainfall[WET];
      energy->AlbedoOver      = 0.;
      (*AlbedoUnder)          = BareAlbedo;
      (*NetLongSnow)          = 0.;
      (*NetShortSnow)         = 0.;
      (*NetShortGrnd)         = 0.;
      (*delta_coverage)       = 0.;
      energy->latent          = 0.;
      energy->latent_sub      = 0.;
      energy->sensible        = 0.;
      curr_snow               = 0;
      snow->store_swq         = 0;
      snow->store_coverage    = 1;
      snow->MELTING           = FALSE;

    }

    if ( store_snowfall > TraceSnow || store_snowfall == 0 ) {
      // reset snow albedo ago if new snow is sufficiently deep
      //fprintf(stdout,"YES: last_snow -> %i, curr_snow -> %i, snowfall -> %f\n", snow->last_snow, curr_snow, store_snowfall);
      snow->last_snow = curr_snow;
    }
    else {
      //fprintf(stdout,"NO:  last_snow -> %i, curr_snow -> %i, snowfall -> %f\n", snow->last_snow, curr_snow, store_snowfall);
      snow->last_snow++;
    }
    
  }
  else {
    
    /*****************************
      No Snow Present or Falling
    *****************************/

    /** Initialize variables **/
    *UnderStory             = 0;
    snow->snow              = FALSE;
    energy->Tfoliage        = air_temp;

    /** Compute Radiation Balance for Bare Surface **/ 
    energy->AlbedoOver   = 0.;
    (*AlbedoUnder)       = BareAlbedo;
    energy->NetLongOver  = 0.;
    energy->LongOverIn   = 0.;
    energy->NetShortOver = 0.;
    energy->ShortOverIn  = 0.;
    energy->latent       = 0.;
    energy->latent_sub   = 0.;
    energy->sensible     = 0.;
    (*NetLongSnow)       = 0.;
    (*NetShortSnow)      = 0.;
    (*NetShortGrnd)      = 0.;
    (*delta_coverage)    = 0.;
    energy->Tfoliage     = Tcanopy;
    snow->store_swq      = 0;
    snow->store_coverage = 1;
    snow->MELTING        = FALSE;
  }

  energy->melt_energy *= -1.;

  return(melt);

}




