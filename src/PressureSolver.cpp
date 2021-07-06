#include "PressureSolver.hpp"
#include "Communication.hpp"

#include <cmath>
#include <iostream>
#include <mpi.h>

SOR::SOR(Real omega) : _omega(omega) {}

Real SOR::solve(Fields& field, Grid& grid, const std::vector<std::unique_ptr<Boundary>>& boundaries, Params& params,
                uint32_t iters, Real tolerance, uint32_t& it) {

   auto step = [&]() -> Real {
        Real dx = grid.dx();
        Real dy = grid.dy();

        Real coeff = _omega / (2.0f * (1.0 / (dx * dx) + 1.0 / (dy * dy))); // = _omega * h^2 / 4.0, if dx == dy == h

        for (auto currentCell : grid.fluid_cells()) {
            int i = currentCell->i();
            int j = currentCell->j();

            field.p(i, j) = (1.0 - _omega) * field.p(i, j) +
                            coeff * (Discretization::sor_helper(field.p_matrix(), i, j) - field.rs(i, j));
        }

        Real res = 0.0;
        Real rloc = 0.0;

        for (auto currentCell : grid.fluid_cells()) {
            int i = currentCell->i();
            int j = currentCell->j();

            Real val = Discretization::laplacian(field.p_matrix(), i, j) - field.rs(i, j);
            rloc += (val * val);
        }
        // Exchange pressure
        Communication::communicate(&params, field.p_matrix());
        // Compute global residual
        int global_cells;
        res = Communication::reduce_all(rloc, MPI_SUM);
        global_cells = Communication::reduce_all(grid.fluid_cells().size(), MPI_SUM);
        {
            res = res / global_cells;
            res = std::sqrt(res);
        }

        return res;
    };
    Real res = REAL_MAX;
    it = 0;
    while (it < iters && res > tolerance) {
        res = step();
        // Enforce boundary conditions
        for (const auto &boundary : boundaries) {
            boundary->enforce_p(field);
        }
        it++;
    }
    return res;
}

PCG::PCG(int dim_x, int dim_y, Real dx, Real dy, Fields &field, Grid &grid,
         const std::vector<std::unique_ptr<Boundary>> &boundaries)
    : dim(dim_x * dim_y), dim_x(dim_x), dim_y(dim_y), A(dim_x * dim_y), U(dim_x * dim_y), V(dim_x * dim_y),
      T(dim_x * dim_y) {
    build_matrix(dx, dy, field, grid, boundaries);
}

Real PCG::solve(Fields &field, Grid &grid, const std::vector<std::unique_ptr<Boundary>> &boundaries, Params &params,
                uint32_t iters, Real tolerance, uint32_t &it) {
    Real pcg_residual = 1e10;
    int pcg_iters = -1;
    static SparsePCGSolver<Real> solver;
    solver.set_solver_parameters(tolerance, iters);
    int dim_x = this->dim_x;
    auto at = [dim_x](int i, int j) { return j * dim_x + i; };
    Real inv_dx2 = 1 / (grid.dx() * grid.dx());
    Real inv_dy2 = 1 / (grid.dy() * grid.dy());
    Real div = inv_dx2 + inv_dy2;
    std::vector<Real> p(dim, 0);

    solver.solve(A, field._RS._container, p, pcg_residual, pcg_iters, 0);
    if (pcg_iters == 0) {
        int a = 4;
    }
    for (int i = 0; i < field._P.size(); i++) {
        field._P._container[i] = -p[i];
    }
    std::cout << pcg_iters;
    // field._P._container = p;
    return pcg_residual;
}

void PCG::build_matrix(Real dx, Real dy, Fields &field, Grid &grid,
                       const std::vector<std::unique_ptr<Boundary>> &boundaries) {
    int dim_x = this->dim_x;
    Real inv_dx2 = 1 / (dx * dx);
    Real inv_dy2 = 1 / (dy * dy);
    Real div = inv_dx2 + inv_dy2;
    auto at = [dim_x](int i, int j) { return j * dim_x + i; };
    /* A.set_element(0, 0, 1);
     A.set_element(at(dim_x - 1, dim_y - 1), at(dim_x - 1, dim_y - 1), 1);
     for (int j = 0; j < dim_y; j++) {
         for (int i = 0; i < dim_x; i++) {
             auto loc = at(i, j);
             A.set_element(loc, loc, 1);
         }
     }*/
    U_RHS.resize(dim);
    V_RHS.resize(dim);

    for (auto current_cell : grid.fluid_cells()) {
        int i = current_cell->i();
        int j = current_cell->j();
        auto loc = at(i, j);
        A.set_element(loc, loc, 2 * div);
        A.set_element(loc, at(i - 1, j), -inv_dx2);
        A.set_element(loc, at(i + 1, j), -inv_dx2);
        A.set_element(loc, at(i, j + 1), -inv_dy2);
        A.set_element(loc, at(i, j - 1), -inv_dy2);
        U.set_element(loc, loc, 1);
        V.set_element(loc, loc, 1);
        T.set_element(loc, loc, 1);
    }
    for (auto &boundary : boundaries) {
        for (auto &cell : *boundary->_cells) {
            int i = cell->i();
            int j = cell->j();
            int id = cell->id();
            auto loc = at(i, j);
            switch (boundary->get_type()) {
            case 0: {
                // Outlet
                A.set_element(loc, loc, 1);
                field.rs(i, j) = field._PI;
            } break;
            case 1: {
                // Inlet
                auto inlet_vel_u = static_cast<InletBoundary *>(boundary.get())->_inlet_U[id];
                auto inlet_vel_v = static_cast<InletBoundary *>(boundary.get())->_inlet_V[id];
                field.rs(i, j) = 0;
                A.set_element(loc, loc, 1 * inv_dx2);
                // Apply noslip for now
                if (cell->is_border(border_position::RIGHT)) {
                    A.set_element(loc, loc, inv_dx2);
                    A.set_element(loc, at(i + 1, j), -inv_dx2);

                } else if (cell->is_border(border_position::LEFT)) {
                    A.set_element(loc, loc, inv_dx2);
                    A.set_element(loc, at(i - 1, j), -inv_dx2);

                } else if (cell->is_border(border_position::TOP)) {
                    A.set_element(loc, loc, inv_dy2);
                    A.set_element(loc, at(i, j + 1), -inv_dy2);

                } else if (cell->is_border(border_position::BOTTOM)) {
                    A.set_element(loc, loc, inv_dy2);
                    A.set_element(loc, at(i, j - 1), -inv_dy2);
                }
                if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::TOP)) {
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i + 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j + 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::BOTTOM)) {
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i + 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j - 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::TOP)) {
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i - 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j + 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::BOTTOM)) {
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i - 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j - 1), -0.5 * inv_dy2);
                }
            } break;
            case 2: {
                // NoSlip
                field.rs(i, j) = 0;
                A.set_element(loc, loc, 1 * inv_dx2);
                auto wall_vel_u = static_cast<NoSlipWallBoundary *>(boundary.get())->_wall_velocity[id];
                auto wall_vel_v = static_cast<NoSlipWallBoundary *>(boundary.get())->_wall_velocity[id];
                // Apply noslip for now
                if (cell->is_border(border_position::RIGHT)) {
                    A.set_element(loc, loc, inv_dx2);
                    A.set_element(loc, at(i + 1, j), -inv_dx2);
                } else if (cell->is_border(border_position::LEFT)) {
                    // Pressure
                    A.set_element(loc, loc, inv_dx2);
                    A.set_element(loc, at(i - 1, j), -inv_dx2);
                } else if (cell->is_border(border_position::TOP)) {
                    // Pressure
                    A.set_element(loc, loc, inv_dy2);
                    A.set_element(loc, at(i, j + 1), -inv_dy2);
                } else if (cell->is_border(border_position::BOTTOM)) {
                    // Pressure
                    A.set_element(loc, loc, inv_dy2);
                    A.set_element(loc, at(i, j - 1), -inv_dy2);
                }
                if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::TOP)) {
                    // Pressure
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i + 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j + 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::BOTTOM)) {
                    // Pressure
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i + 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j - 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::TOP)) {
                    // Pressure
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i - 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j + 1), -0.5 * inv_dy2);
                }
                if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::BOTTOM)) {
                    // Pressure
                    A.set_element(loc, loc, 0.5 * (div));
                    A.set_element(loc, at(i - 1, j), -0.5 * inv_dx2);
                    A.set_element(loc, at(i, j - 1), -0.5 * inv_dy2);
                }
            } break;
            case 3: {
                // FreeSlip
            } break;
            default:
                break;
            }
        }
    }

    auto zero_val = [at](SparseMatrix<Real> &M, std::vector<Real> &RHS, int i, int j) {
        M.set_element(at(i, j), at(i, j), 0);
        /*  M.set_element(at(i, j), at(i + 1, j), 0);
          M.set_element(at(i, j), at(i - 1, j), 0);
          M.set_element(at(i, j), at(i, j - 1), 0);
          M.set_element(at(i, j), at(i, j + 1), 0);*/
        RHS[at(i, j)] = 0;
    };

    for (auto &boundary : boundaries) {
        for (auto &cell : *boundary->_cells) {
            int i = cell->i();
            int j = cell->j();
            int id = cell->id();
            auto loc = at(i, j);
            switch (boundary->get_type()) {
            case 0: {
                // Outlet

            } break;
            case 1: {
                // Inlet
                auto inlet_vel_u = static_cast<InletBoundary *>(boundary.get())->_inlet_U[id];
                auto inlet_vel_v = static_cast<InletBoundary *>(boundary.get())->_inlet_V[id];

                // Apply noslip for now
                if (cell->is_border(border_position::RIGHT)) {

                    // Velocity
                    V.set_element(loc, at(i + 1, j), -1);
                    U_RHS[loc] += 0.5 * inlet_vel_u;
                } else if (cell->is_border(border_position::LEFT)) {

                    // Velocity
                    V.set_element(loc, at(i - 1, j), -1);
                    U_RHS[loc] += 0.5 * inlet_vel_u;
                } else if (cell->is_border(border_position::TOP)) {

                    // Velocity
                    U.set_element(loc, at(i, j + 1), -1);
                    V_RHS[loc] += 0.5 * inlet_vel_v;
                } else if (cell->is_border(border_position::BOTTOM)) {

                    // Velocity
                    U.set_element(loc, at(i, j - 1), -1);
                    V_RHS[loc] += 0.5 * inlet_vel_v;
                }

            } break;
            case 2: {
                // NoSlip

                auto wall_vel_u = static_cast<NoSlipWallBoundary *>(boundary.get())->_wall_velocity[id];
                auto wall_vel_v = static_cast<NoSlipWallBoundary *>(boundary.get())->_wall_velocity[id];
                // Apply noslip for now
                if (cell->borders().size() == 1) {
                    if (cell->is_border(border_position::RIGHT)) {

                        // Velocity
                        V_RHS[loc] += wall_vel_v;
                        zero_val(U, U_RHS, i, j);
                        V.set_element(loc, at(i + 1, j), -1);
                    } else if (cell->is_border(border_position::LEFT)) {

                        // Velocity
                        V_RHS[loc] += wall_vel_v;
                        zero_val(U, U_RHS, i - 1, j);
                        V.set_element(loc, at(i - 1, j), -1);
                    } else if (cell->is_border(border_position::TOP)) {

                        // Velocity
                        U_RHS[loc] += wall_vel_u;
                        zero_val(V, V_RHS, i, j);
                        U.set_element(loc, at(i, j + 1), -1);
                    } else if (cell->is_border(border_position::BOTTOM)) {

                        // Velocity
                        U_RHS[loc] += wall_vel_u;
                        zero_val(V, V_RHS, i, j - 1);
                        U.set_element(loc, at(i, j - 1), -1);
                    }
                } else if (cell->borders().size() == 2) {
                    if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::TOP)) {

                        // Velocity
                        U_RHS[at(i - 1, j)] += wall_vel_u;
                        V_RHS[at(i, j - 1)] += wall_vel_v;
                        zero_val(U, U_RHS, i, j);
                        zero_val(V, V_RHS, i, j);
                        U.set_element(at(i - 1, j), at(i - 1, j + 1), -1);
                        V.set_element(at(i, j - 1), at(i + 1, j - 1), -1);
                    } else if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::BOTTOM)) {

                        // Velocity
                        U_RHS[at(i - 1, j)] += wall_vel_u;
                        V_RHS[at(i, j)] += wall_vel_v;
                        zero_val(U, U_RHS, i, j);
                        zero_val(V, V_RHS, i, j - 1);
                        U.set_element(at(i - 1, j), at(i - 1, j - 1), -1);
                        V.set_element(loc, at(i + 1, j), -1);
                    } else if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::TOP)) {

                        // Velocity
                        U_RHS[loc] += wall_vel_u;
                        V_RHS[at(i, j - 1)] += wall_vel_v;
                        zero_val(U, U_RHS, i - 1, j);
                        zero_val(V, V_RHS, i, j);
                        U.set_element(loc, at(i, j + 1), -1);
                        V.set_element(at(i, j - 1), at(i - 1, j - 1), -1);
                    } else if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::BOTTOM)) {

                        zero_val(U, U_RHS, i - 1, j);
                        zero_val(V, V_RHS, i, j - 1);
                        // Velocity
                        U_RHS[loc] += wall_vel_u;
                        V_RHS[loc] += wall_vel_v;
                        U.set_element(loc, at(i, j - 1), -1);
                        V.set_element(loc, at(i - 1, j), -1);
                    }
                }
            } break;
            case 3: {
                // FreeSlip
                if (cell->borders().size() == 1) {
                    if (cell->is_border(border_position::RIGHT)) {
                        // Velocity
                        zero_val(U, U_RHS, i, j);
                    } else if (cell->is_border(border_position::LEFT)) {
                        zero_val(U, U_RHS, i - 1, j);
                    } else if (cell->is_border(border_position::TOP)) {
                        zero_val(V, V_RHS, i, j);
                    } else if (cell->is_border(border_position::BOTTOM)) {
                        zero_val(V, V_RHS, i, j - 1);
                    }
                }
            } break;
            default:
                break;
            }
        }
    }
    if (field.calc_temp) {
        T_RHS.resize(dim);
        for (auto &boundary : boundaries) {
            for (auto &cell : *boundary->_cells) {
                int i = cell->i();
                int j = cell->j();
                int id = cell->id();
                auto loc = at(i, j);
                switch (boundary->get_type()) {
                case 0: {
                    // Outlet
                    if (cell->is_border(border_position::RIGHT)) {
                        // T
                        T.set_element(loc, at(i + 1, j), 1);
                    } else if (cell->is_border(border_position::LEFT)) {
                        // T
                        T.set_element(loc, at(i - 1, j), 1);
                    } else if (cell->is_border(border_position::TOP)) {
                        // T
                        T.set_element(loc, at(i, j + 1), 1);
                    } else if (cell->is_border(border_position::BOTTOM)) {
                        // T
                        T.set_element(loc, at(i, j - 1), 1);
                    }
                } break;
                case 1: {
                    // Inlet
                    auto wt = static_cast<InletBoundary *>(boundary.get())->_wall_temperature[id];
                    T_RHS[loc] += wt;
                    if (cell->is_border(border_position::RIGHT)) {
                        // T
                        T.set_element(loc, at(i + 1, j), -1);
                    } else if (cell->is_border(border_position::LEFT)) {
                        // T
                        T.set_element(loc, at(i - 1, j), -1);
                    } else if (cell->is_border(border_position::TOP)) {
                        // T
                        T.set_element(loc, at(i, j + 1), -1);
                    } else if (cell->is_border(border_position::BOTTOM)) {
                        // T
                        T.set_element(loc, at(i, j - 1), -1);
                    }
                }

                break;
                case 2: {
                    // NoSlip
                    auto wt = static_cast<InletBoundary *>(boundary.get())->_wall_temperature[id];
                    if (cell->is_border(border_position::RIGHT)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -1);
                        } else {
                            T.set_element(loc, at(i + 1, j), 1);
                        }
                    } else if (cell->is_border(border_position::LEFT)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -1);
                        } else {
                            T.set_element(loc, at(i - 1, j), 1);
                        }
                    } else if (cell->is_border(border_position::TOP)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i, j + 1), -1);
                        } else {
                            T.set_element(loc, at(i, j + 1), 1);
                        }
                    } else if (cell->is_border(border_position::BOTTOM)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i, j - 1), -1);
                        } else {
                            T.set_element(loc, at(i, j - 1), 1);
                        }
                    }
                    if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::TOP)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -0.5);
                            T.set_element(loc, at(i, j + 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::BOTTOM)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -0.5);
                            T.set_element(loc, at(i, j - 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::TOP)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -0.5);
                            T.set_element(loc, at(i, j + 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::BOTTOM)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -0.5);
                            T.set_element(loc, at(i, j - 1), -0.5);
                        } else {
                        }
                    }
                } break;
                case 3: {
                    // FreeSlip
                    auto wt = static_cast<InletBoundary *>(boundary.get())->_wall_temperature[id];
                    if (cell->is_border(border_position::RIGHT)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -1);
                        } else {
                            T.set_element(loc, at(i + 1, j), 1);
                        }
                    } else if (cell->is_border(border_position::LEFT)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -1);
                        } else {
                            T.set_element(loc, at(i - 1, j), 1);
                        }
                    } else if (cell->is_border(border_position::TOP)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i, j + 1), -1);
                        } else {
                            T.set_element(loc, at(i, j + 1), 1);
                        }
                    } else if (cell->is_border(border_position::BOTTOM)) {
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i, j - 1), -1);
                        } else {
                            T.set_element(loc, at(i, j - 1), 1);
                        }
                    }
                    if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::TOP)) {
                        // T
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -0.5);
                            T.set_element(loc, at(i, j + 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::RIGHT) && cell->is_border(border_position::BOTTOM)) {
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i + 1, j), -0.5);
                            T.set_element(loc, at(i, j - 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::TOP)) {
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -0.5);
                            T.set_element(loc, at(i, j + 1), -0.5);
                        } else {
                        }
                    }
                    if (cell->is_border(border_position::LEFT) && cell->is_border(border_position::BOTTOM)) {
                        if (wt != -1) {
                            T_RHS[loc] += wt;
                            T.set_element(loc, at(i - 1, j), -0.5);
                            T.set_element(loc, at(i, j - 1), -0.5);
                        } else {
                        }
                    }
                } break;
                default:
                    break;
                }
            }
        }
    }
    U_fixed.construct_from_matrix(U);
    V_fixed.construct_from_matrix(V);
    T_fixed.construct_from_matrix(T);
}
