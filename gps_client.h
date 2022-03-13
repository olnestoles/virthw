/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   gsp_client.h
 * Author: oles2
 *
 * Created on October 14, 2020, 5:49 PM
 */

#ifndef GSP_CLIENT_H
#define GSP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

int update_gps_hw(char *new_data);
int init_gps_client();


#ifdef __cplusplus
}
#endif

#endif /* GSP_CLIENT_H */

