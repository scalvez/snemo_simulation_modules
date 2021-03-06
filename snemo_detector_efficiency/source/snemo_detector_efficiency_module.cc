/* snemo_detector_efficiency_module.cc
 *
 * Copyright (C) 2013 Xavier Garrido <garrido@lal.in2p3.fr>

 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include <stdexcept>
#include <sstream>
#include <set>
#include <numeric>

// #include <boost/bind.hpp>

#include <snemo_detector_efficiency_module.h>

// Third party:
// - Bayeux/datatools:
#include <datatools/service_manager.h>
// - Bayeux/geomtools:
#include <geomtools/geometry_service.h>
#include <geomtools/manager.h>

// SuperNEMO event model
#include <falaise/snemo/datamodels/data_model.h>
#include <falaise/snemo/datamodels/calibrated_data.h>
#include <falaise/snemo/datamodels/tracker_trajectory.h>
#include <falaise/snemo/processing/services.h>
// #include <snanalysis/models/data_model.h>
// #include <snanalysis/models/particle_track_data.h>

// Geometry manager
#include <falaise/snemo/geometry/calo_locator.h>
#include <falaise/snemo/geometry/xcalo_locator.h>
#include <falaise/snemo/geometry/gveto_locator.h>
#include <falaise/snemo/geometry/gg_locator.h>

namespace analysis {

  // Registration instantiation macro :
  DPP_MODULE_REGISTRATION_IMPLEMENT(snemo_detector_efficiency_module,
                                    "analysis::snemo_detector_efficiency_module");

  void snemo_detector_efficiency_module::set_geometry_manager(const geomtools::manager & gmgr_)
  {
    DT_THROW_IF (is_initialized(), std::logic_error,
                 "Module '" << get_name() << "' is already initialized ! ");
    _geometry_manager_ = &gmgr_;

    // Check setup label:
    const std::string & setup_label = _geometry_manager_->get_setup_label ();
    DT_THROW_IF(setup_label.find("snemo::") == std::string::npos,
                std::logic_error,
                "Setup label '" << setup_label << "' is not supported !");
    return;
  }

  const geomtools::manager & snemo_detector_efficiency_module::get_geometry_manager() const
  {
    return *_geometry_manager_;
  }

  void snemo_detector_efficiency_module::_set_defaults ()
  {
    _bank_label_ = "";

    _Geo_service_label_ = "";
    _geometry_manager_  = 0;

    _calo_locator_.reset ();
    _xcalo_locator_.reset ();
    _gveto_locator_.reset ();

    _calo_efficiencies_.clear ();
    _gg_efficiencies_.clear ();

    return;
  }

  /*** Implementation of the interface ***/

  // Constructor :
  snemo_detector_efficiency_module::snemo_detector_efficiency_module(datatools::logger::priority logging_priority_)
    : dpp::base_module(logging_priority_)
  {
    _set_defaults();
    return;
  }

  // Destructor :
  snemo_detector_efficiency_module::~snemo_detector_efficiency_module()
  {
    // Make sure all internal resources are terminated
    // before destruction :
    if (is_initialized ()) reset ();
    return;
  }

  // Reset :
  void snemo_detector_efficiency_module::reset()
  {
    DT_THROW_IF (! is_initialized (),
                 std::logic_error,
                 "Module '" << get_name() << "' is not initialized !");

    // Compute efficiency
    _compute_efficiency();

    // Dump result
    dump_result(std::clog,
                "analysis::snemo_detector_efficiency_module::dump_result: ",
                "NOTICE: ");

    _set_defaults();

    // Tag the module as un-initialized :
    _set_initialized (false);
    return;
  }

  // Initialization :
  void snemo_detector_efficiency_module::initialize(const datatools::properties  & config_,
                                                    datatools::service_manager   & service_manager_,
                                                    dpp::module_handle_dict_type & module_dict_)
  {
    DT_THROW_IF (is_initialized(),
                 std::logic_error,
                 "Module '" << get_name() << "' is already initialized ! ");

    dpp::base_module::_common_initialize(config_);

    if (config_.has_key("bank_label"))
      {
        _bank_label_ = config_.fetch_string("bank_label");
      }

    // Geometry manager :
    if (_geometry_manager_ == 0) {
      std::string geo_label = snemo::processing::service_info::default_geometry_service_label();
      if (config_.has_key("Geo_label")) {
        geo_label = config_.fetch_string("Geo_label");
      }
      DT_THROW_IF (geo_label.empty(), std::logic_error,
                   "Module '" << get_name() << "' has no valid '" << "Geo_label" << "' property !");
      DT_THROW_IF (! service_manager_.has(geo_label) ||
                   ! service_manager_.is_a<geomtools::geometry_service>(geo_label),
                   std::logic_error,
                   "Module '" << get_name() << "' has no '" << geo_label << "' service !");
      geomtools::geometry_service & Geo
        = service_manager_.get<geomtools::geometry_service>(geo_label);
      set_geometry_manager(Geo.get_geom_manager());
    }

    // Initialize locators
    const int mn = 0;
    _calo_locator_.reset  (new snemo::geometry::calo_locator  (get_geometry_manager (), mn));
    _xcalo_locator_.reset (new snemo::geometry::xcalo_locator (get_geometry_manager (), mn));
    _gveto_locator_.reset (new snemo::geometry::gveto_locator (get_geometry_manager (), mn));
    _gg_locator_.reset    (new snemo::geometry::gg_locator    (get_geometry_manager (), mn));

    // Tag the module as initialized :
    _set_initialized (true);
    return;
  }

  // Processing :
  dpp::base_module::process_status
  snemo_detector_efficiency_module::process(datatools::things & data_record_)
  {
    DT_THROW_IF (! is_initialized(), std::logic_error,
                 "Module '" << get_name() << "' is not initialized !");

    namespace sdm = snemo::datamodel;
    if (_bank_label_ == sdm::data_info::CALIBRATED_DATA_LABEL)
      {
        // Check if the 'calibrated data' record bank is available :
        if (!data_record_.has(_bank_label_))
          {
            DT_LOG_ERROR(get_logging_priority(),
                         "Could not find any bank with label '" << _bank_label_ << "' !");
            return dpp::base_module::PROCESS_STOP;
          }
        const sdm::calibrated_data & cd
          = data_record_.get<sdm::calibrated_data>(_bank_label_);

        const sdm::calibrated_data::calorimeter_hit_collection_type & the_calo_hits
          = cd.calibrated_calorimeter_hits ();
        const sdm::calibrated_data::tracker_hit_collection_type & the_tracker_hits
          = cd.calibrated_tracker_hits ();

        for (sdm::calibrated_data::calorimeter_hit_collection_type::const_iterator
               i = the_calo_hits.begin ();
             i != the_calo_hits.end (); ++i)
          {
            const sdm::calibrated_calorimeter_hit & a_hit = i->get ();
            _calo_efficiencies_[a_hit.get_geom_id ()]++;
          }
        for (sdm::calibrated_data::tracker_hit_collection_type::const_iterator
               i = the_tracker_hits.begin ();
             i != the_tracker_hits.end (); ++i)
          {
            const sdm::calibrated_tracker_hit & a_hit = i->get ();
            _gg_efficiencies_[a_hit.get_geom_id ()]++;
          }
      }
    // else if (_bank_label_ == snemo::analysis::model::data_info::PARTICLE_TRACK_DATA_LABEL)
    //   {
    //     // Check if the 'particle track' record bank is available :
    //     if (!data_record_.has(_bank_label_))
    //       {
    //         DT_LOG_ERROR(get_logging_priority(),
    //                      "Could not find any bank with label '" << _bank_label_ << "' !");
    //         return STOP;
    //       }
    //     const snemo::analysis::model::particle_track_data & ptd
    //       = data_record_.get<snemo::analysis::model::particle_track_data>(_bank_label_);

    //     // Store geom_id to avoid double inclusion of calorimeter hits
    //     std::set<geomtools::geom_id> gids;

    //     // Loop over all saved particles
    //     const snemo::analysis::model::particle_track_data::particle_collection_type & the_particles
    //       = ptd.get_particles ();

    //     for (snemo::analysis::model::particle_track_data::particle_collection_type::const_iterator
    //            iparticle = the_particles.begin ();
    //          iparticle != the_particles.end ();
    //          ++iparticle)
    //       {
    //         const snemo::analysis::model::particle_track & a_particle = iparticle->get ();

    //         if (!a_particle.has_associated_calorimeters ()) continue;

    //         const snemo::analysis::model::particle_track::calorimeter_collection_type & the_calorimeters
    //           = a_particle.get_associated_calorimeters ();

    //         if (the_calorimeters.size () > 2)
    //           {
    //             DT_LOG_DEBUG(get_logging_priority(),
    //                          "The particle is associated to more than 2 calorimeters !");
    //             continue;
    //           }

    //         for (size_t i = 0; i < the_calorimeters.size (); ++i)
    //           {
    //             const geomtools::geom_id & a_gid = the_calorimeters.at (i).get ().get_geom_id ();
    //             if (gids.find (a_gid) != gids.end ()) continue;
    //             gids.insert (a_gid);
    //             _calo_efficiencies_[a_gid]++;
    //           }

    //         // Get trajectory and attached geiger cells
    //         const sdm::tracker_trajectory & a_trajectory = a_particle.get_trajectory ();
    //         const sdm::tracker_cluster & a_cluster = a_trajectory.get_cluster ();
    //         const sdm::calibrated_tracker_hit::collection_type & the_hits = a_cluster.get_hits ();
    //         for (size_t i = 0; i < the_hits.size (); ++i)
    //           {
    //             const geomtools::geom_id & a_gid = the_hits.at (i).get ().get_geom_id ();
    //             if (gids.find (a_gid) != gids.end ()) continue;
    //             gids.insert (a_gid);
    //             _gg_efficiencies_[a_gid]++;
    //           }
    //       }
    //   }
    else
      {
        DT_THROW_IF(true, std::logic_error,
                    "Bank label '" << _bank_label_ << "' is not supported !");
      }
    return dpp::base_module::PROCESS_SUCCESS;
  }

  void snemo_detector_efficiency_module::_compute_efficiency() {}
  // {
  //   // Handling geom_id is done in this place where geom_id are split into
  //   // main wall, xwall and gveto calorimeters. For such task we use
  //   // sngeometry locators

  //   std::ofstream fout ("/tmp/efficiency.dat");
  //   {
  //     efficiency_dict::const_iterator found =
  //       std::max_element (_calo_efficiencies_.begin (), _calo_efficiencies_.end (),
  //                         (boost::bind(&efficiency_dict::value_type::second, _1) <
  //                          boost::bind(&efficiency_dict::value_type::second, _2)));
  //     const int calo_total = found->second;

  //     for (efficiency_dict::const_iterator i = _calo_efficiencies_.begin ();
  //          i != _calo_efficiencies_.end (); ++i)
  //       {
  //         const geomtools::geom_id & a_gid = i->first;

  //         if (_calo_locator_->is_calo_block_in_current_module (a_gid))
  //           {
  //             fout << "calo ";
  //             geomtools::vector_3d position;
  //             _calo_locator_->get_block_position (a_gid, position);
  //             fout << position.x () << " "
  //                  << position.y () << " "
  //                  << position.z () << " ";
  //           }
  //         else if (_xcalo_locator_->is_calo_block_in_current_module (a_gid))
  //           {
  //             fout << "xcalo ";
  //             geomtools::vector_3d position;
  //             _xcalo_locator_->get_block_position (a_gid, position);
  //             fout << position.x () << " "
  //                  << position.y () << " "
  //                  << position.z () << " ";
  //           }
  //         else if (_gveto_locator_->is_calo_block_in_current_module (a_gid))
  //           {
  //             fout << "gveto ";
  //             geomtools::vector_3d position;
  //             _gveto_locator_->get_block_position (a_gid, position);
  //             fout << position.x () << " "
  //                  << position.y () << " "
  //                  << position.z () << " ";
  //           }
  //         fout << i->second/double (calo_total) << std::endl;
  //       }
  //   }

  //   {
  //     efficiency_dict::const_iterator found =
  //       std::max_element (_gg_efficiencies_.begin (), _gg_efficiencies_.end (),
  //                         (boost::bind(&efficiency_dict::value_type::second, _1) <
  //                          boost::bind(&efficiency_dict::value_type::second, _2)));
  //     const int gg_total = found->second;

  //     for (efficiency_dict::const_iterator i = _gg_efficiencies_.begin ();
  //          i != _gg_efficiencies_.end (); ++i)
  //       {
  //         const geomtools::geom_id & a_gid = i->first;

  //         if (_gg_locator_->is_drift_cell_volume_in_current_module (a_gid))
  //           {
  //             fout << "gg ";
  //             geomtools::vector_3d position;
  //             _gg_locator_->get_cell_position (a_gid, position);
  //             fout << position.x () << " " << position.y () << " ";
  //           }
  //         fout << i->second/double (gg_total) << std::endl;
  //       }
  //   }

  //   return;
  // }

  void snemo_detector_efficiency_module::dump_result(std::ostream      & out_,
                                                     const std::string & title_,
                                                     const std::string & indent_,
                                                     bool inherit_) const
  {
    std::string indent;
    if (! indent_.empty ())
      {
        indent = indent_;
      }
    if ( !title_.empty () )
      {
        out_ << indent << title_ << std::endl;
      }

    {
      for (efficiency_dict::const_iterator i = _calo_efficiencies_.begin ();
           i != _calo_efficiencies_.end (); ++i)
        {
          // out_ << indent << datatools::utils::i_tree_dumpable::tag
          //      << i->first << " = " << i->second << std::endl;
        }
    }

    return;
  }

} // namespace analysis

// end of snemo_detector_efficiency_module.cc
/*
** Local Variables: --
** mode: c++ --
** c-file-style: "gnu" --
** tab-width: 2 --
** End: --
*/
