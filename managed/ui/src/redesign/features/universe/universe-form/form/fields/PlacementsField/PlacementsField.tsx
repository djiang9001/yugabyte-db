import React, { ReactElement } from 'react';
import { useTranslation } from 'react-i18next';
import { useFormContext, useFieldArray, useWatch, Controller } from 'react-hook-form';
import { Box, Typography, MenuItem, makeStyles } from '@material-ui/core';
import { YBButton, YBSelect, YBLabel, YBCheckbox, YBInput } from '../../../../../../components';
import { YBLoadingCircleIcon } from '../../../../../../../components/common/indicators';
import { PlacementStatus } from './PlacementStatus';
import { useGetAllZones, useGetUnusedZones, useNodePlacements } from './PlacementsFieldHelper';
import { Placement, UniverseFormData, CloudType, MasterPlacementMode } from '../../../utils/dto';
import {
  REPLICATION_FACTOR_FIELD,
  PLACEMENTS_FIELD,
  PROVIDER_FIELD,
  RESET_AZ_FIELD,
  MASTER_PLACEMENT_FIELD
} from '../../../utils/constants';
import { useFormFieldStyles } from '../../../universeMainStyle';

interface PlacementsFieldProps {
  disabled: boolean;
  isPrimary: boolean;
}

// Override MuiFormControl style to ensure flexDirection is inherited
// and this ensures all the columns are aligned at the same level
const useStyles = makeStyles((theme) => ({
  overrideMuiFormControl: {
    '& .MuiFormControl-root': {
      flexDirection: 'inherit'
    }
  },
  nameColumn: {
    width: 400,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'flex-start'
  },
  nodesColumn: {
    width: 100,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'flex-start',
    margin: theme.spacing(0, 1)
  },
  preferredColumn: {
    width: 100,
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'flex-start'
  }
}));

//Extended for useFieldArray
export type PlacementWithId = Placement & { id: any };

const DEFAULT_MIN_NUM_NODE = 1;
export const PlacementsField = ({ disabled, isPrimary }: PlacementsFieldProps): ReactElement => {
  const { control, setValue, getValues } = useFormContext<UniverseFormData>();
  const { t } = useTranslation();
  const classes = useFormFieldStyles();
  const helperClasses = useStyles();

  //watchers
  const replicationFactor = useWatch({ name: REPLICATION_FACTOR_FIELD });
  const provider = useWatch({ name: PROVIDER_FIELD });
  const masterPlacement = useWatch({ name: MASTER_PLACEMENT_FIELD });

  //custom hooks
  const allZones = useGetAllZones(); //returns all AZ
  const unUsedZones = useGetUnusedZones(allZones); //return unused AZ
  const { isLoading } = useNodePlacements(); // Places Nodes

  const { fields, update, append } = useFieldArray({
    control,
    name: PLACEMENTS_FIELD
  });
  const isDedicatedNodes = masterPlacement === MasterPlacementMode.DEDICATED;

  const renderHeader = (
    <Box flex={1} mb={1} display="flex" flexDirection="row" data-testid="PlacementsField-Container">
      <Box className={helperClasses.nameColumn}>
        <YBLabel dataTestId="PlacementsField-AZNameLabel">
          {t('universeForm.cloudConfig.azNameLabel')}
        </YBLabel>
      </Box>
      <Box className={helperClasses.nodesColumn}>
        <YBLabel dataTestId="PlacementsField-IndividualUnitLabel">
          {provider?.code === CloudType.kubernetes
            ? t('universeForm.cloudConfig.azPodsLabel')
            : isDedicatedNodes
            ? t('universeForm.cloudConfig.azTServerNodesLabel')
            : t('universeForm.cloudConfig.azNodesLabel')}
        </YBLabel>
      </Box>
      {isPrimary && (
        <Box className={helperClasses.preferredColumn}>
          <YBLabel dataTestId="PlacementsField-PreferredLabel">
            {t('universeForm.cloudConfig.preferredAZLabel')}
          </YBLabel>
        </Box>
      )}
    </Box>
  );

  const handleAZChange = (oldAz: Placement, azName: string, index: any) => {
    const selectedZone = allZones.find((az: any) => az.name === azName);
    const updateAz = { ...oldAz, ...selectedZone };
    update(index, updateAz);
  };

  //get Minimum AZ count to update before making universe_configure call
  const getMinCountAZ = (index: number) => {
    const initialCount = 0;
    const totalNodesinAz = fields
      .map((e) => e.numNodesInAZ)
      .reduce((prev, cur) => prev + cur, initialCount);
    const min = fields[index].numNodesInAZ - (totalNodesinAz - getValues(REPLICATION_FACTOR_FIELD));
    return min > 0 ? min : DEFAULT_MIN_NUM_NODE;
  };

  const renderPlacements = () => {
    return fields.map((field: PlacementWithId, index: number) => {
      const prefferedAZField = `${PLACEMENTS_FIELD}.${index}.isAffinitized` as any;

      return (
        <Box flex={1} display="flex" mb={2} flexDirection="row" key={field.id}>
          <Box className={helperClasses.nameColumn}>
            <YBSelect
              fullWidth
              disabled={isLoading}
              value={field.name}
              inputProps={{
                'data-testid': `PlacementsField-AZName${index}`
              }}
              onChange={(e) => {
                handleAZChange(field, e.target.value, index);
              }}
            >
              {[field, ...unUsedZones].map((az) => (
                <MenuItem key={az.name} value={az.name}>
                  {az.name}
                </MenuItem>
              ))}
            </YBSelect>
          </Box>
          <Box className={helperClasses.nodesColumn}>
            <Controller
              control={control}
              name={`${PLACEMENTS_FIELD}.${index}.numNodesInAZ` as const}
              render={({ field: { onChange, ref, ...rest } }) => (
                <YBInput
                  type="number"
                  fullWidth
                  inputRef={ref}
                  disabled={isLoading}
                  onChange={(e) => {
                    if (!e.target.value || Number(e.target.value) < getMinCountAZ(index))
                      onChange(getMinCountAZ(index));
                    else onChange(Number(e.target.value));
                  }}
                  {...rest}
                  inputProps={{
                    'data-testid': `PlacementsField-IndividualCount${index}`
                  }}
                />
              )}
            />
          </Box>
          {isPrimary && (
            <Box className={helperClasses.preferredColumn}>
              <YBCheckbox
                name={prefferedAZField}
                onChange={(e) => {
                  setValue(prefferedAZField, e.target.checked);
                }}
                defaultChecked={field.isAffinitized}
                value={field.isAffinitized}
                disabled={isLoading}
                label=""
                inputProps={{
                  'data-testid': `PlacementsField-PrefferedCheckbox${index}`
                }}
              />
            </Box>
          )}
        </Box>
      );
    });
  };

  if (fields.length) {
    return (
      <Box
        display="flex"
        width="100%"
        flexDirection="column"
        data-testid="PlacementsField-Container"
      >
        <Box width="100%" display="flex" flexDirection="row" mb={1.5} alignItems={'center'}>
          <Box flexShrink={1} mr={3}>
            <Typography variant="h4">{t('universeForm.cloudConfig.azHeader')}</Typography>
          </Box>
          <YBButton variant="secondary" onClick={() => setValue(RESET_AZ_FIELD, true)}>
            {t('universeForm.cloudConfig.resetAZLabel')}
          </YBButton>
        </Box>
        {renderHeader}
        {renderPlacements()}
        {unUsedZones.length > 0 && fields.length < replicationFactor && (
          <Box display="flex" justifyContent={'flex-start'} mr={0.5} mt={1}>
            <YBButton
              style={{ width: '150px' }}
              variant="primary"
              disabled={isLoading}
              data-testid="PlacementsField-AddAZButton"
              onClick={() =>
                append({
                  ...unUsedZones[0],
                  numNodesInAZ: 1,
                  replicationFactor: 1,
                  isAffinitized: true
                })
              }
            >
              {t('universeForm.cloudConfig.addZoneButton')}
            </YBButton>
          </Box>
        )}
        <PlacementStatus />
      </Box>
    );
  }

  if (isLoading)
    return (
      <Box
        display="flex"
        marginTop={15}
        alignItems={'center'}
        flexDirection={'column'}
        data-testid="PlacementsField-Loader"
      >
        <YBLoadingCircleIcon size="small" />
        Loading placements
      </Box>
    );
  return <></>;
};
